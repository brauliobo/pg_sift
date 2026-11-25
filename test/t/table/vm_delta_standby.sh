#!/usr/bin/env bash
# A hot standby with hot_standby_feedback = off reads the rows of a sift_table by key while the primary changes them with column deltas, VACUUMs and maintains the table, with every page of the
# WAL checked against the page the replay makes (wal_consistency_checking = all). The standby replays the records that recycle the pages of a chain of column deltas while a statement reads
# the head page of it to know whether the map it kept for the next fetch of a row still holds: a page that is not the one the map was made of is a retry (40001), never damage. No statement
# of the primary fails, nothing of the standby is damaged or quarantined and the standby counts, afterwards, what the primary does.
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_VS_ROWS 20000 10000)
SECS=$(knob SIFT_VS_SECS 10 5)
plan 6
node_init primary 0 "shared_preload_libraries = 'pg_sift'
wal_level = replica
max_wal_senders = 4
wal_keep_size = 512MB
wal_consistency_checking = 'all'
sift.test_create_format1 = off
sift.async_maintenance = off
sift.log_min_duration = 0
autovacuum = off"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" "CREATE TABLE t (id int PRIMARY KEY, k int, v int, a int, b int, c int, d int, e int, f int, pad text) USING sift_table" \
	"INSERT INTO t SELECT g, g, g, g, g, g, g, g, g, md5(g::text) FROM generate_series(1, $ROWS) g" "ANALYZE t" "VACUUM t" >/dev/null || bail "setup failed"
node_clone primary standby 1 "hot_standby_feedback = off
wal_receiver_status_interval = 1s
max_standby_streaming_delay = 1s"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"
P=$(dir primary)
S=$(dir standby)
cat >"$P/upd.sql" <<SQL
\set rid random(1, $ROWS)
UPDATE t SET v = v + 1 WHERE id = :rid;
SQL
cat >"$S/read.sql" <<SQL
\set rid random(1, $((ROWS - 400)))
SELECT sum(t.v) FROM generate_series(1, 200) g JOIN t ON t.id = :rid + g * 2;
SQL

maintainer() { while [[ ! -e $P/stop ]]; do sql primary "VACUUM t" "SELECT sift.maintain('t')" >>"$P/maintain.n" 2>>"$P/maintain.err" || echo failed >>"$P/maintain.err"; done; }
rm -f "$P/stop"
: >"$P/maintain.err" >"$P/maintain.n"
maintainer & mp=$!
pgbench_bg primary 4 "$SECS" -f "$P/upd.sql"
primary_bench=$PGBENCH_PID
pgbench -n -h "$(node_sock standby)" -p "$(port standby)" -U postgres -c 4 -j 2 -T "$SECS" --verbose-errors -f "$S/read.sql" postgres >"$S/pgbench.log" 2>&1 &
standby_bench=$!
wait "$primary_bench"; primary_status=$?
wait "$standby_bench"
touch "$P/stop"
wait "$mp"
diag "$(grep -h -E 'transactions actually processed|number of failed' "$P/pgbench.log" "$S/pgbench.log" | tr '\n' ' ') maintenance passes: $(wc -l <"$P/maintain.n" | tr -d ' ')"

DAMAGE='terminated by signal|stack depth|is damaged|is corrupted|quarantine|TRAP|PANIC|inconsistent page'
check "pgbench ran its clients of the primary to the end" test "$primary_status" -eq 0
check "no statement of the primary failed, VACUUM and the maintenance ran beside them" test "$(grep -ac -E '^(pgbench: error|ERROR)|failed transactions: [1-9]' "$P/pgbench.log")" -eq 0 -a ! -s "$P/maintain.err"
check "the standby read the rows beside them" test "$(sed -n 's/.*number of transactions actually processed: \([0-9]*\).*/\1/p' "$S/pgbench.log")" -gt 0
out=$(grep -a "ERROR" "$S/pgbench.log" | grep -a -v -E 'retry|conflict with recovery|canceling statement due to conflict' | sort | uniq -c | head)
check "what a statement of the standby fails with is a retry or a conflict with recovery" test -z "$out"
out=$(grep -a -m3 -E "$DAMAGE" "$P/postgres.log" "$S/postgres.log")
check "nothing is damaged, quarantined or inconsistent on the primary or the standby (logs of the servers)" test -z "$out"
catchup primary standby || bail "the standby did not catch up"
check "the standby counts and sums what the primary does" test "$(sql standby "SELECT count(*), sum(v) FROM t")" == "$(sql primary "SELECT count(*), sum(v) FROM t")"
finish
