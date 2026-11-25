#!/usr/bin/env bash
# 16 clients write a sift_table (60% INSERT, 30% UPDATE of one column, 10% DELETE) while VACUUM and sift.maintain run on it again and again. An UPDATE of a few columns is a column delta staged
# in the transaction; a VACUUM or a maintenance step that replaces the part of a staged row gives the flush of the stage a part to look the row up in, and that lookup must not flush the
# stage it is flushing (it once did, to the end of the stack: the server crashed within seconds). No statement fails, the server does not restart, nothing is quarantined, the table checks,
# and the aggregates answer what the rows of a scan do.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 6
SECS=$(knob SIFT_VW_SECS 10 5)
node_init vw 24 "autovacuum = off
sift.async_maintenance = off
sift.log_min_duration = 0"
node_start vw || bail "start failed"
TESTING=$(sql vw "SELECT count(*) FROM pg_settings WHERE name = 'sift.test_create_format1'")
F2=$([[ $TESTING == 1 ]] && echo "SET sift.test_create_format1 = off" || echo "SELECT 1")
sql vw "CREATE EXTENSION pg_sift" "$F2" "CREATE TABLE facts (id bigint PRIMARY KEY, key int, kind text, amount int, pad text) USING sift_table" \
	"CREATE SEQUENCE facts_seq START 30001" \
	"INSERT INTO facts SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 40) FROM generate_series(1, 30000) g" "VACUUM facts" >/dev/null || bail "setup failed"
D=$(dir vw)
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

maintainer() { while [[ ! -e $D/stop ]]; do sql vw "VACUUM facts" "SELECT sift.maintain('facts')" >>"$D/maintain.n" 2>>"$D/maintain.err" || echo failed >>"$D/maintain.err"; done; }

rm -f "$D/stop"
: >"$D/maintain.err" >"$D/maintain.n"
maintainer & mp=$!
pgbench_bg vw 16 "$SECS" -D n=60000 -f "$D/ins.sql"@6 -f "$D/upd.sql"@3 -f "$D/del.sql"@1
wait "$PGBENCH_PID"; bench=$?
touch "$D/stop"
wait "$mp"
diag "$(grep -E 'transactions actually processed|tps' "$D/pgbench.log" | tr '\n' ' ') maintenance passes: $(wc -l <"$D/maintain.n" | tr -d ' ')"
diag "first trouble in the log: $(grep -a -m3 -E 'terminated by signal|stack depth|is damaged|is corrupted|quarantine|TRAP' "$D/postgres.log" | cut -c1-240)"$'\n'"maintainer: $(head -n 3 "$D/maintain.err" | cut -c1-240)"
check "pgbench ran its 16 clients to the end" test "$bench" -eq 0
check "no statement of the 16 clients failed" test "$(grep -ac -E '^(pgbench: error|ERROR)|failed transactions: [1-9]' "$D/pgbench.log")" -eq 0
check "VACUUM and the maintenance ran beside them and failed no statement" test ! -s "$D/maintain.err" -a "$(wc -l <"$D/maintain.n")" -ge 3
check "the server never crashed and nothing is damaged or quarantined (log of the server)" test "$(grep -ac -E 'terminated by signal|stack depth|is damaged|is corrupted|quarantine|TRAP' "$D/postgres.log")" -eq 0
check "afterwards the table checks" test -z "$(sql vw "VACUUM facts" "SELECT problem FROM sift.check('facts')" 2>&1)"
check "the aggregates count and sum the rows of a scan" test "$(sql vw "SET sift.enable_agg = on" "SELECT count(*), sum(amount) FROM facts" | tail -1)" == "$(sql vw "SET sift.enable_agg = off" "SELECT count(*), sum(amount) FROM facts" | tail -1)"
finish
