#!/usr/bin/env bash
# The VACUUM a read asks for, priced by the read penalty (sift.eval_methods = 'penalty_maintenance', core/eval.h). Without the method a churn of 0.1% of the rows starts the worker from
# the UPDATE that made it; with it the writes ask for nothing, a scan of a thousand rows neither, and a VACUUM is asked for by the reads that have checked rows in the heap for half of what it costs. A few reads do not
# come to that, enough of them do, and the answers are the table's either way.
. "$(dirname "$0")/lib.bash"

plan 6
node_init rd 25 "autovacuum = off
sift.maintenance_naptime = 200"
node_start rd || bail "start failed"
sql rd "CREATE EXTENSION pg_sift" "CREATE TABLE facts (id int PRIMARY KEY, key int, v int)" \
	"INSERT INTO facts SELECT g, g % 300, g % 1000 FROM generate_series(1, 20000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, v)" "VACUUM (ANALYZE, FREEZE, DISABLE_PAGE_SKIPPING) facts" >/dev/null || bail "setup failed"
sql rd "ALTER DATABASE postgres SET sift.col_auto_merge = on" "ALTER DATABASE postgres SET sift.eval_methods = 'penalty_maintenance'" >/dev/null

vacuums() { sql rd "SELECT vacuum_count FROM pg_stat_user_tables WHERE relname = 'facts'"; }
asked() { for _ in $(seq 100); do [[ $(vacuums) -gt $1 ]] && return 0; sleep 0.3; done; return 1; }
# $1 batches of 10 grouped reads of the whole table in one session, a naptime apart: the look of the tick is once per naptime
reads() { sql rd "SET sift.enable_agg = on" "SET max_parallel_workers_per_gather = 0" \
	"DO \$\$ BEGIN FOR b IN 1..$1 LOOP FOR i IN 1..10 LOOP PERFORM key, count(*), sum(v) FROM facts GROUP BY key; END LOOP; PERFORM pg_sleep(0.25); END LOOP; END \$\$" >/dev/null; }
churn() { sql rd "UPDATE facts SET v = v + 1 WHERE id % 2 = $1" >/dev/null; }

base=$(vacuums)
churn 0
sleep 3
check "with the method on the writes ask for no VACUUM" test "$(vacuums)" == "$base"
reads 3
sleep 2
check "and a few reads do not" test "$(vacuums)" == "$base"
reads 40
check "the reads that have checked rows for half of the price ask for it" asked "$base"
check "the answers are the table's" test "$(sql rd "SELECT sum(v) FROM facts WHERE key < 30")" == "$(sql rd "SET sift.enable_agg = off" "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SET enable_indexonlyscan = off" "SELECT sum(v) FROM facts WHERE key < 30" | tail -1)"

sql rd "ALTER DATABASE postgres SET sift.eval_methods = ''" >/dev/null
base=$(vacuums)
churn 1
reads 3
check "with the list empty the same few reads ask for it, by the rule of a thousand rows" asked "$base"
check "the counters of the read penalty are in sift.stats" test "$(sql rd "SELECT count(*) FROM sift.stats() WHERE name IN ('rows_dirty_units', 'read_penalty_kinstr', 'maintenance_asks')")" == 3
finish
