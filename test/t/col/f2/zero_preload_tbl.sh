#!/usr/bin/env bash
# A sift_table of format major 2 on a server that does not preload pg_sift (shared_preload_libraries = '': no shared memory of the library, no resource manager for the visibility map, no
# launcher), beside a heap twin that takes the same statements: COPY, narrow UPDATEs (column deltas), DELETE, VACUUM (FREEZE), the maintenance, a PRIMARY KEY of the table and a UNIQUE
# sift_col index, each held against a duplicate by one session and by two concurrent ones, two-phase commit (commit, rollback, and a transaction that stays prepared across a crash), the clean
# refusal of a write to a published table (wal_level = logical), and a crash with its replay. The table answers what its twin answers at every step and sift.check finds nothing.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 20
node_init zp 1 "shared_preload_libraries = ''
max_prepared_transactions = 4
wal_level = logical
autovacuum = off"
node_start zp || bail "start failed"
D=$(dir zp)
COLS="id int PRIMARY KEY, k int NOT NULL, v int NOT NULL, a int, pad text NOT NULL"
sql zp "CREATE EXTENSION pg_sift" "CREATE TABLE t ($COLS) USING sift_table" "CREATE TABLE h ($COLS)" \
	"CREATE TABLE u (id int, v int)" "CREATE UNIQUE INDEX u_id ON u USING sift_col (id)" >/dev/null || bail "setup failed"

both() { local s; for s in t h; do sql zp "${1//@T/$s}" || return; done; }
digest() { tap_digest zp "$1" id "id || ',' || k || ',' || v || ',' || coalesce(a, 0) || ',' || pad"; }
same() { [[ $(digest t) == "$(digest h)" ]]; }
problems() { sql zp "SELECT count(*) FROM sift.check('t', true)"; }
errors() { sql zp "$1" 2>&1 | grep -c "$2"; }

check "pg_sift is not preloaded" test -z "$(sql zp "SHOW shared_preload_libraries")"
sql zp "COPY (SELECT g, g % 13, g % 100, g % 5, md5(g::text) FROM generate_series(1, 20000) g) TO '$D/seed.csv' CSV" >/dev/null
both "COPY @T FROM '$D/seed.csv' CSV" >/dev/null
check "a COPY of 20,000 rows gives the rows of the heap twin" test "$(digest t)" == "$(digest h)" -a "$(sql zp "SELECT count(*) FROM t")" == 20000

sql zp "UPDATE t SET v = v + 1 WHERE id % 7 = 0" "UPDATE h SET v = v + 1 WHERE id % 7 = 0" >/dev/null
check "a narrow UPDATE is a column delta: it deletes no row" test "$(sql zp "SELECT dead_rows FROM sift.space('t')")" == 0 -a "$(digest t)" == "$(digest h)"
both "UPDATE @T SET a = a + 1, v = v + 2 WHERE id % 3 = 0" >/dev/null
both "DELETE FROM @T WHERE id % 11 = 0" >/dev/null
both "INSERT INTO @T SELECT g, g % 13, g % 100, NULL, md5(g::text) FROM generate_series(20001, 24000) g" >/dev/null
check "UPDATEs, DELETE and INSERT give the rows of the twin, by the table and by the key" test "$(digest t)" == "$(digest h)" \
	-a "$(sql zp "SELECT v FROM t WHERE id = 2100")" == "$(sql zp "SELECT v FROM h WHERE id = 2100")"

both "VACUUM (FREEZE) @T" >/dev/null
check "VACUUM (FREEZE) passes without the resource manager of the visibility map and the rows are the twin's" same
diag "visibility map fork of t after VACUUM (FREEZE): $(sql zp "SELECT pg_relation_size('t', 'vm')") bytes"
sql zp "SELECT count(*) FROM sift.maintain('t')" >/dev/null
check "sift.maintain runs in the session and the table verifies" test "$(problems)" == 0 -a "$(digest t)" == "$(digest h)"

check "a duplicate key of the table is refused" test "$(errors "INSERT INTO t VALUES (1, 1, 1, 1, 'dup')" 'duplicate key')" == 1
race zp "INSERT INTO t VALUES (900001, 1, 1, 1, 'first')" "INSERT INTO t VALUES (900001, 2, 2, 2, 'second')"
check "two sessions that insert one key of the table: the second waits for the first and fails with a duplicate key when it commits" test "$RACE_WAITED" == 1 \
	-a "$(grep -c 'duplicate key' "$D/rb.out")" == 1 -a "$(sql zp "SELECT pad FROM t WHERE id = 900001")" == first
sql zp "DELETE FROM t WHERE id = 900001" >/dev/null

sql zp "INSERT INTO u VALUES (1, 1)" >/dev/null
check "a duplicate key of a UNIQUE sift_col index is refused" test "$(errors "INSERT INTO u VALUES (1, 2)" 'duplicate key')" == 1
rm -f "$D"/r?.out
race zp "INSERT INTO u VALUES (5, 1)" "INSERT INTO u VALUES (5, 2)"
check "two sessions that insert one key of a UNIQUE sift_col index: one row, one refusal" test "$(sql zp "SELECT count(*) FROM u WHERE id = 5")" == 1 -a "$(grep -c 'duplicate key' "$D/rb.out")" == 1

sql zp "BEGIN" "INSERT INTO t VALUES (910001, 1, 1, 1, 'p1')" "PREPARE TRANSACTION 'p1'" >/dev/null
check "a prepared transaction shows nothing before its commit" test "$(sql zp "SELECT count(*) FROM t WHERE id = 910001")" == 0 -a "$(sql zp "SELECT count(*) FROM pg_prepared_xacts")" == 1
sql zp "COMMIT PREPARED 'p1'" "INSERT INTO h VALUES (910001, 1, 1, 1, 'p1')" >/dev/null
check "COMMIT PREPARED shows its row" test "$(sql zp "SELECT pad FROM t WHERE id = 910001")" == p1
sql zp "BEGIN" "INSERT INTO t VALUES (910002, 1, 1, 1, 'p2')" "UPDATE t SET v = v + 1000 WHERE id = 5" "PREPARE TRANSACTION 'p2'" >/dev/null
sql zp "ROLLBACK PREPARED 'p2'" >/dev/null
check "ROLLBACK PREPARED leaves neither its row nor its update" test "$(sql zp "SELECT count(*) FROM t WHERE id = 910002")" == 0 -a "$(digest t)" == "$(digest h)"

check "CREATE PUBLICATION of a sift_table is refused, naming the library" test "$(errors "CREATE PUBLICATION pb FOR TABLE t" 'is not in shared_preload_libraries')" == 1
sql zp "CREATE PUBLICATION ph FOR TABLE h" >/dev/null
check "so is every other way to publish it (all tables, a schema, ALTER PUBLICATION), and the heap twin is published" \
	test "$(errors "CREATE PUBLICATION pa FOR ALL TABLES" 'is not in shared_preload_libraries')" == 1 \
	-a "$(errors "CREATE PUBLICATION ps FOR TABLES IN SCHEMA public" 'is not in shared_preload_libraries')" == 1 \
	-a "$(errors "ALTER PUBLICATION ph ADD TABLE t" 'is not in shared_preload_libraries')" == 1 -a "$(sql zp "SELECT string_agg(pubname, ',') FROM pg_publication")" == ph
both "INSERT INTO @T VALUES (920002, 1, 1, 1, 'unpublished')" >/dev/null
check "the table takes writes after the refusals and answers as its twin" same

sql zp "BEGIN" "INSERT INTO t VALUES (930001, 1, 1, 1, 'across')" "PREPARE TRANSACTION 'p3'" >/dev/null
both "UPDATE @T SET v = v + 1 WHERE id % 5 = 0" >/dev/null
both "DELETE FROM @T WHERE id % 17 = 0" >/dev/null
BEFORE=$(digest t)
wal_flushed zp
crash zp
node_start zp || bail "the server did not come back from the crash"
check "after a crash and the replay of the WAL the table answers what it answered before and what its twin does" test "$(digest t)" == "$BEFORE" -a "$(digest h)" == "$BEFORE"
check "the table verifies after the recovery" test "$(problems)" == 0
check "the transaction that was prepared at the crash is still prepared and commits" test "$(sql zp "SELECT count(*) FROM pg_prepared_xacts")" == 1 \
	-a "$(sql zp "COMMIT PREPARED 'p3'" "SELECT pad FROM t WHERE id = 930001" | tail -n 1)" == across
both "VACUUM @T" >/dev/null
out=$(grep -a -m3 -E 'TRAP|PANIC|terminated by signal|stack depth|is damaged|quarantine' "$D/postgres.log")
check "the log of the server has no crash, damage or quarantine" test -z "$out"
finish
