#!/usr/bin/env bash
# The members of a parallel build (a leader and workers, one lock group) intern the strings of a shared value store at the same time. Only the relation extension lock conflicts between the
# members of a lock group (PostgreSQL's lock.c, LockCheckConflicts; the group locking section of src/backend/storage/lmgr/README): the appends of the store are made under it, so two members never hand out the
# same code. The same table is built by the participants and by one process: the stores hold the same strings once each (as many codes as distinct values, a duplicate code would add one),
# every code reads back its string, and the answers are the heap's and are the same for both.
. "$(dirname "$0")/lib.bash"

plan 8
node_init vp 36 "autovacuum = off
sift.async_maintenance = off
sift.col_auto_merge = off
max_worker_processes = 16
max_parallel_workers = 8
max_parallel_maintenance_workers = 4"
node_start vp || bail "start failed"
ROWS=$(knob SIFT_VSTORE_PAR_ROWS 60000 30000)
sql vp "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"
for t in par ser; do
	sql vp "CREATE TABLE $t (id int PRIMARY KEY, url text, v int) WITH (autovacuum_enabled = off)" \
		"INSERT INTO $t SELECT g, 'u' || (g * 7 % ($ROWS / 3)), g % 100 FROM generate_series(1, $ROWS) g" \
		"ALTER TABLE $t SET (parallel_workers = 4)" "VACUUM (FREEZE, ANALYZE) $t" >/dev/null || bail "setup of $t failed"
done
build() { # TABLE WORKERS: the indexes of the table built with that many parallel maintenance workers; the workers launched by the last build
	sql vp "SET sift.test_vstore_columns = 'url'" "SET max_parallel_maintenance_workers = $2" "SET min_parallel_table_scan_size = 0" "SET sift.test_col_parallel_always = on" \
		"SET sift.test_col_segment_kb = 32" "CREATE INDEX $1_a ON $1 USING sift_col (id, url, v)" "CREATE INDEX $1_b ON $1 USING sift_col (url, id, v)" "SHOW sift.test_col_workers" | tail -1
}
LAUNCHED=$(build par 4)
check "the participants build the indexes (workers launched: $LAUNCHED)" test "$LAUNCHED" -ge 1
build ser 0 >/dev/null
DISTINCT=$(sql vp "SELECT count(DISTINCT url) FROM par")
codes() { sql vp "SELECT view_values FROM sift._test_vstore_info('$1_url_sift_values')"; }
check "the store of the parallel build holds one code for each of the $DISTINCT distinct strings ($(codes par))" test "$(codes par)" == "$DISTINCT"
check "the store of the serial build holds the same ($(codes ser))" test "$(codes ser)" == "$DISTINCT"
check "every code of the parallel build reads back a string, and no two codes read the same" test "$(sql vp "SELECT count(DISTINCT sift._test_vstore_value('par_url_sift_values', c)) FROM generate_series(0, $DISTINCT - 1) c")" == "$DISTINCT"
ANS="SELECT md5(string_agg(x::text, '|' ORDER BY x::text)) FROM (SELECT url, count(*), sum(v) FROM TBL GROUP BY url) x"
answers() { sql vp "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "${ANS//TBL/$1}" | tail -1; }
heap() { sql vp "SET sift.enable_agg = off" "SET max_parallel_workers_per_gather = 0" "${ANS//TBL/$1}" | tail -1; }
check "the parallel build gives the heap's answers" test "$(answers par)" == "$(heap par)"
check "the serial build gives the heap's answers" test "$(answers ser)" == "$(heap ser)"
check "the two builds give the same answers" test "$(answers par)" == "$(answers ser)"
check "both indexes verify" test -z "$(sql vp "SELECT sift.col_verify('par_a', true)" "SELECT sift.col_verify('par_b', true)" 2>&1 | grep -i -E "error|corrupt")"
finish
