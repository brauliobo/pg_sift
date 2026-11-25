#!/usr/bin/env bash
# 16 clients write a heap table with a sift_col index (60% INSERT, 30% UPDATE of one column, 10% DELETE) while VACUUM and sift.maintain run on it again and again. The lazy VACUUM of
# the index (ambulkdelete) reads the slab and the part list of every partition while the writers and the maintenance replace them: it must never follow a pointer to a block that was
# freed and reused (it once did: "expected page type 33 ... found type 38" while vacuuming the index, the index quarantined; and a double free in sift.maintain). No statement fails, the
# server does not restart, nothing is quarantined, the index checks, and the aggregates answer what the rows of a scan do.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 6
SECS=$(knob SIFT_VW_SECS 10 5)
node_init ivw 24 "autovacuum = off
sift.async_maintenance = off
sift.log_min_duration = 0"
node_start ivw || bail "start failed"
TESTING=$(sql ivw "SELECT count(*) FROM pg_settings WHERE name = 'sift.test_create_format1'")
F2=$([[ $TESTING == 1 ]] && echo "SET sift.test_create_format1 = off" || echo "SELECT 1")
sql ivw "CREATE EXTENSION pg_sift" "$F2" "CREATE TABLE facts (id bigint PRIMARY KEY, key int, kind text, amount int, pad text) WITH (autovacuum_enabled = false)" \
	"CREATE SEQUENCE facts_seq START 30001" \
	"INSERT INTO facts SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 40) FROM generate_series(1, 30000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount)" "VACUUM facts" >/dev/null || bail "setup failed"
D=$(dir ivw)
cat >"$D/ins.sql" <<'SQL'
INSERT INTO facts SELECT i, i % 1000, 'cat_' || (i % 8), i % 997, repeat('x', 40) FROM (SELECT nextval('facts_seq') AS i) n;
SQL
cat >"$D/upd.sql" <<'SQL'
\set rid random(1, :n)
UPDATE facts SET amount = amount + 1 WHERE id = :rid;
SQL
cat >"$D/del.sql" <<'SQL'
\set rid random(1, :n)
DELETE FROM facts WHERE id = :rid;
SQL

maintainer() { while [[ ! -e $D/stop ]]; do sql ivw "VACUUM facts" "SELECT sift.maintain('facts_idx')" >>"$D/maintain.n" 2>>"$D/maintain.err" || echo failed >>"$D/maintain.err"; done; }

rm -f "$D/stop"
: >"$D/maintain.err" >"$D/maintain.n"
maintainer & mp=$!
pgbench_bg ivw 16 "$SECS" -D n=60000 -f "$D/ins.sql"@6 -f "$D/upd.sql"@3 -f "$D/del.sql"@1
wait "$PGBENCH_PID"; bench=$?
touch "$D/stop"
wait "$mp"
diag "$(grep -E 'transactions actually processed|tps' "$D/pgbench.log" | tr '\n' ' ') maintenance passes: $(wc -l <"$D/maintain.n" | tr -d ' ')"
trouble() { grep -a -v 'listening on Unix socket' "$D/postgres.log" | grep -a -E 'terminated by signal|double free|is damaged|is corrupted|quarantine|TRAP'; }
diag "first trouble in the log: $(trouble | head -n 3 | cut -c1-240)"$'\n'"maintainer: $(head -n 3 "$D/maintain.err" | cut -c1-240)"
check "pgbench ran its 16 clients to the end" test "$bench" -eq 0
check "no statement of the 16 clients failed" test "$(grep -ac -E '^(pgbench: error|ERROR)|failed transactions: [1-9]' "$D/pgbench.log")" -eq 0
check "VACUUM and the maintenance ran beside them and failed no statement" test ! -s "$D/maintain.err" -a "$(wc -l <"$D/maintain.n")" -ge 3
check "the server never crashed and nothing is damaged or quarantined (log of the server)" test "$(trouble | wc -l)" -eq 0
check "afterwards the index checks" test -z "$(sql ivw "VACUUM facts" "SELECT problem FROM sift.check('facts_idx')" 2>&1)"
check "the aggregates count and sum the rows of a scan" test "$(sql ivw "SET sift.enable_agg = on" "SELECT count(*), sum(amount) FROM facts" | tail -1)" == "$(sql ivw "SET sift.enable_agg = off" "SELECT count(*), sum(amount) FROM facts" | tail -1)"
finish
