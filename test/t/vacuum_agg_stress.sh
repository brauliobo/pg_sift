#!/usr/bin/env bash
# Groups of the aggregate pushdown with a text search (SiftAgg) against the groups of the heap in the same REPEATABLE READ snapshot, while
# deletes, updates, inserts, VACUUM and merges of a second session: they are equal. The column index of the table is created before the
# BM25 index, so a VACUUM cleans it first and a scan that begins between the two passes reads the match set of the second before it
# is cleaned. Runs on the release build too (no test functions), with less reach.
. "$(dirname "$0")/lib.bash"

SECS=$(knob SIFT_CHURN_SECS 40 15)
ROWS=100000
plan 3
node_init vg 19 "autovacuum = off
max_worker_processes = 16
shared_buffers = 256MB"
node_start vg || bail "start failed"
D=$(dir vg)
sql vg "CREATE EXTENSION pg_sift" "CREATE TABLE st (id serial, body text, cat text, n int)" \
	"INSERT INTO st (body, cat, n) SELECT 'zzword w' || i || ' x' || (i % 97), 'c' || (i % 7), i % 100 FROM generate_series(1, $ROWS) i" \
	"CREATE INDEX st_col ON st USING sift_col (cat, n)" "CREATE INDEX st_idx ON st USING sift (body) WITH (write_buffer_limit = 4096, build_segments = 4)" "VACUUM ANALYZE st" \
	"CREATE TABLE diffs (agg text, heap text)" \
	"CREATE FUNCTION chk() RETURNS void LANGUAGE plpgsql AS \$\$ DECLARE a text; b text; BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('sift.enable_scan', 'off', true);
	EXECUTE 'SELECT string_agg(cat || ''='' || c || ''/'' || s, '','' ORDER BY cat) FROM (SELECT cat, count(*) c, sum(n) s FROM st WHERE body @@> ''x7'' GROUP BY cat) g' INTO a;
	PERFORM set_config('sift.enable_agg', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	EXECUTE 'SELECT string_agg(cat || ''='' || c || ''/'' || s, '','' ORDER BY cat) FROM (SELECT cat, count(*) c, sum(n) s FROM st WHERE body ~ ''(^| )x7( |\$)'' GROUP BY cat) g' INTO b;
	IF a IS DISTINCT FROM b THEN INSERT INTO diffs VALUES (a, b); END IF;
END \$\$" >/dev/null || bail "setup failed"
check "the aggregate runs as SiftAgg with the search" test "$(sql vg "SET enable_seqscan = off" "SET sift.enable_scan = off" "EXPLAIN (COSTS OFF) SELECT cat, count(*) FROM st WHERE body @@> 'x7' GROUP BY cat" | grep -c "SiftAgg")" == 1

# a testing build also sleeps up to a millisecond before each read of the map, which widens the window the aggregate has to be wrong in
printf '%s\n' "LOAD 'pg_sift';" "SELECT current_setting('sift.test_vis_jitter_us', true) IS NOT NULL AS jit \\gset" '\if :jit' 'SET sift.test_vis_jitter_us = 1000;' '\endif' \
	'BEGIN ISOLATION LEVEL REPEATABLE READ;' 'SELECT chk();' 'COMMIT;' >"$D/read.sql"
pgbench_bg vg 1 "$SECS" -f "$D/read.sql"

# the writes and the VACUUM are one session, so that every VACUUM has rows deleted just before it for the readers to be wrong about
n=0
: >"$D/maint.out"
while kill -0 "$PGBENCH_PID" 2>/dev/null; do
	sql vg "DELETE FROM st WHERE id % 1000 = $((RANDOM % 1000)) AND id <= $ROWS" "UPDATE st SET body = body || ' u' WHERE id % 1000 = $((RANDOM % 1000)) AND id <= $ROWS" \
		"INSERT INTO st (body, cat, n) SELECT 'zzword n' || i || ' x' || (i % 97), 'c' || (i % 7), i % 100 FROM generate_series(1, 200) i" "VACUUM st" >>"$D/maint.out" 2>&1 || echo FAILED >>"$D/maint.out"
	(( n % 2 )) || sql vg "SELECT sift.merge('st_idx', true)" >>"$D/maint.out" 2>&1 || echo FAILED >>"$D/maint.out"
	(( ++n % 5 )) || sql vg "SELECT sift.promote('st_idx')" >>"$D/maint.out" 2>&1 || echo FAILED >>"$D/maint.out"
done
wait "$PGBENCH_PID"
diag "$(grep -E "^(number of transactions actually|tps)" "$D/pgbench.log")"
diag "$n rounds of VACUUM and merge"
check "no groups differed from the heap's in its snapshot" test "$(sql vg "SELECT count(*) FROM diffs")" == 0
check "VACUUM and the merges ran without error" test "$(grep -c -E "FAILED|ERROR" "$D/maint.out")" == 0
[[ $(sql vg "SELECT count(*) FROM diffs") == 0 ]] || diag "$(sql vg "SELECT * FROM diffs LIMIT 5")"
finish
