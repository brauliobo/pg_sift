#!/usr/bin/env bash
# Durability of what sift writes and of what it takes out of service, in four cases: (1) a part written into a file that a commit made already goes through the shared buffers and generic WAL (no
# full-page record of smgr_bulk_write ahead of its WAL), while a file this transaction creates keeps the bulk run; (2) a quarantined index stays out of service whatever sift.on_corruption says,
# across a restart, with the flag in its metapage written before the write that skips the index, and a hot standby's session forgets the flag it read when the flag is written; (3) VACUUM
# VERBOSE and sift.col_maintain open a part in object storage by its descriptor; (4) a logical slot with no publication decodes the changes of a sift_table.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 17
node_init p 3 "wal_level = replica
max_wal_senders = 5
autovacuum = off
synchronous_commit = on
sift.on_corruption = quarantine"
node_start p || bail "start failed"
D=$(dir p)
sql p "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "CREATE EXTENSION pg_walinspect" \
	"CREATE TABLE w (id int, v int)" "INSERT INTO w SELECT i, i % 7 FROM generate_series(1, 100000) i" \
	"CREATE TABLE m (id int, v int)" "INSERT INTO m SELECT i, i % 7 FROM generate_series(1, 20000) i" "CREATE INDEX m_idx ON m USING sift_col (id, v)" \
	"CREATE TABLE q (id int, v int)" "INSERT INTO q SELECT i, i % 7 FROM generate_series(1, 20000) i" "CREATE INDEX q_idx ON q USING sift_col (id, v)" \
	"ANALYZE m" "ANALYZE q" >/dev/null || bail "setup failed"
node_clone p s 4 "sift.on_corruption = quarantine"
node_start s || bail "standby start failed"
catchup p s || bail "standby catchup failed"

# WAL records of the full-page kind that name two blocks or more of the file of REL between two positions: what smgr_bulk_write sends ahead of the WAL flush of its pages (log_newpage_buffer logs one)
full_pages() { sql p "SELECT count(*) FROM pg_get_wal_records_info('$1', '$2') WHERE resource_manager = 'XLOG' AND record_type = 'FPI' AND block_ref LIKE '%/$(sql p "SELECT pg_relation_filenode('$3')") %' AND block_ref LIKE '%blkref #1:%'"; }
lsn() { sql p "SELECT pg_current_wal_flush_lsn()"; }
no_bulk_pages() { local from=$(lsn); sql p "$1" >/dev/null || return 1; [[ $(full_pages "$from" "$(lsn)" "$2") == 0 ]]; }

# 1: files that exist before the statement
from=$(lsn)
sql p "CREATE INDEX CONCURRENTLY w_cic ON w USING sift_col (id, v)" >/dev/null || bail "CREATE INDEX CONCURRENTLY failed"
check "CREATE INDEX CONCURRENTLY writes no bulk full-page records into the committed index file" test "$(full_pages "$from" "$(lsn)" w_cic)" == 0
sql p "CREATE TABLE c (id int, v int)" "INSERT INTO c SELECT i, i % 7 FROM generate_series(1, 1000) i" "CREATE INDEX c_idx ON c USING sift_col (id, v)" \
	"INSERT INTO c SELECT i, i % 7 FROM generate_series(1001, 300000) i" >/dev/null || bail "setup of c failed"
check "sift.col_maintain rewrites parts without bulk full-page records" no_bulk_pages "SELECT sift.col_maintain('c_idx')" c_idx
sql p "CREATE TABLE st (id int, v int) USING sift_table" "COPY (SELECT i, i % 7 FROM generate_series(1, 300000) i) TO '$D/st.csv' CSV" >/dev/null || bail "setup of st failed"
check "a bulk COPY into a sift_table writes no bulk full-page records into its committed file" no_bulk_pages "COPY st FROM '$D/st.csv' CSV" st
from=$(lsn)
sql p "CREATE INDEX w_new ON w USING sift_col (id, v)" >/dev/null || bail "CREATE INDEX failed"
check "an index file the statement creates keeps the bulk run (control)" test "$(full_pages "$from" "$(lsn)" w_new)" -gt 0

# 2: quarantine
poke() { sql p "SELECT sift._test_poke('$1_idx', 3, 8176, 9, 2)" "CHECKPOINT" >/dev/null || bail "poke of $1 failed"; }
want=$(sql p "SELECT count(*) FROM generate_series(1, 20000) i WHERE i % 7 = 3")
uses_sift() { sql "$1" "EXPLAIN (COSTS OFF) SELECT count(*) FROM $2 WHERE v = 3" | grep -qE "Sift|$2_idx"; }
check "q: the index is planned before the damage" uses_sift p q
poke q
out=$(psql -X -At -q -h "$D/sock" -p "$(port p)" -U postgres -d postgres 2>&1 <<'SQL'
BEGIN;
SAVEPOINT s;
SELECT count(*) FROM q WHERE v = 3;
ROLLBACK TO s;
INSERT INTO q SELECT 20000 + i, 3 FROM generate_series(1, 100) i;
SELECT 'flag ' || persisted FROM sift.quarantine WHERE index = 'q_idx'::regclass;
COMMIT;
SQL
) || true
check "the write that skips the quarantined index has its flag in the metapage before it goes on" grep -qx 'flag true' <<<"$out"
want=$((want + 100))
heap_count() { [[ $(sql p "SET sift.on_corruption = error" "SELECT count(*) FROM q WHERE v = 3") == "$want" ]] && ! sql p "SET sift.on_corruption = error" "EXPLAIN (COSTS OFF) SELECT count(*) FROM q WHERE v = 3" | grep -qE "Sift|q_idx"; }
check "q: sift.on_corruption = error does not bring the quarantined index back" heap_count
node_stop p
node_conf p "sift.on_corruption = error"
node_start p || bail "restart failed"
check "q: after a restart under sift.on_corruption = error the flag keeps the index out of service" test "$(sql p "SELECT persisted FROM sift.quarantine WHERE index = 'q_idx'::regclass")" == t
check "q: and the statements run on the heap with its rows" heap_count
sql p "REINDEX INDEX q_idx" >/dev/null || bail "REINDEX failed"
check "q: REINDEX takes it out of the list" test "$(sql p "SELECT count(*) FROM sift.quarantine WHERE index = 'q_idx'::regclass")" == 0
node_stop p
node_conf p "sift.on_corruption = quarantine"
node_start p || bail "restart failed"

sess_open s ss
sess_say ss "EXPLAIN (COSTS OFF) SELECT count(*) FROM m WHERE v = 3; \\echo planned"
wait_for "grep -q '^planned\$' $(dir s)/ss.out" 15
poke m
sql p "SELECT count(*) FROM m WHERE v = 3" >/dev/null 2>&1 || true
flagged() { [[ $(sql p "SELECT persisted FROM sift.quarantine WHERE index = 'm_idx'::regclass") == t ]]; }
wait_for flagged 30
catchup p s || bail "standby catchup failed"
sess_say ss "EXPLAIN (COSTS OFF) SELECT count(*) FROM m WHERE v = 3; \\echo replanned"
wait_for "grep -q '^replanned\$' $(dir s)/ss.out" 15
before=$(sed -n '1,/^planned$/p' "$(dir s)/ss.out")
after=$(sed -n '/^planned$/,/^replanned$/p' "$(dir s)/ss.out")
sess_close ss
check "a session of the standby that planned the index plans without it once the primary flags it" \
	bash -c 'grep -qE "Sift|m_idx" <<<"$1" && ! grep -qE "Sift|m_idx" <<<"$2"' _ "$before" "$after"

# 3: a part in object storage
sql p "CREATE TABLE evi (id int8, ts timestamp NOT NULL, v int)" \
	"INSERT INTO evi SELECT i, timestamp '2020-01-01' + i * interval '1 hour', i % 100 FROM generate_series(1, 5000) i" "CREATE INDEX evi_col ON evi USING sift_col (id, ts, v)" "VACUUM evi" \
	"INSERT INTO evi SELECT 5000 + i, localtimestamp - i * interval '1 minute', i % 100 FROM generate_series(1, 5000) i" >/dev/null || bail "setup of evi failed"
mkdir -p "$D/tier"
sql p "SELECT sift.set_tier_policy('evi_col', 'file://$D/tier', '30 days')" "VACUUM evi" >/dev/null || bail "tiering failed"
check "a part is in object storage" test "$(sql p "SELECT count(*) > 0 FROM sift.tier_status")" == t
out=$(sql p "SELECT sift.col_maintain('evi_col')" 2>&1) || true
check "sift.col_maintain counts the parts of an index with a part in object storage" test "$out" == t
out=$(sql p "VACUUM VERBOSE evi" 2>&1) || true
check "VACUUM VERBOSE of a table whose index has a part in object storage" grep -q 'finished vacuuming' <<<"$out"

# 4: a logical slot and no publication
if [[ ! -f $(pg_config --pkglibdir)/test_decoding.so ]]; then skip 3 "test_decoding is not built"; finish; fi
node_init l 5 "wal_level = logical
shared_preload_libraries = 'pg_sift'
max_replication_slots = 5
autovacuum = off"
node_start l || bail "start of l failed"
sql l "CREATE EXTENSION pg_sift" "SELECT pg_create_logical_replication_slot('s102', 'test_decoding')" "CREATE TABLE lt (id int PRIMARY KEY, v int) USING sift_table" \
	"INSERT INTO lt VALUES (1, 1), (2, 2)" "UPDATE lt SET v = 20 WHERE id = 2" "DELETE FROM lt WHERE id = 1" >/dev/null || bail "setup of lt failed"
out=$(sql l "SELECT data FROM pg_logical_slot_get_changes('s102', NULL, NULL)")
for op in INSERT UPDATE DELETE; do check "the slot decodes the $op of a sift_table" grep -q "table public.lt: $op:" <<<"$out"; done
finish
