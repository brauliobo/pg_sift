#!/usr/bin/env bash
# count(*) through the index against a sequential count in the same REPEATABLE READ snapshot, while deleters, updaters, inserters, VACUUM
# and merges run side by side: the two counts are equal. A VACUUM that removes the index entries of tuples deleted before the snapshot and
# marks their pages all-visible while a count has read the liveness and not yet the map made the index count more rows than the snapshot
# holds (2.0.2: about one query in 100000, a few per minute under 6 readers). Runs on the release build too (no test functions).
. "$(dirname "$0")/lib.bash"

SECS=$(knob SIFT_CHURN_SECS 40 15)
ROWS=300000
plan 3
node_init vs 17 "autovacuum = off
max_worker_processes = 16
shared_buffers = 256MB"
node_start vs || bail "start failed"
D=$(dir vs)
sql vs "CREATE EXTENSION pg_sift" "CREATE TABLE st (id serial, body text)" \
	"INSERT INTO st (body) SELECT 'zzword w' || i || ' x' || (i % 97) FROM generate_series(1, $ROWS) i" \
	"CREATE INDEX st_idx ON st USING sift (body) WITH (mutable_limit = 4096, build_segments = 4)" "VACUUM st" \
	"CREATE TABLE diffs (idx bigint, seq bigint)" \
	"CREATE FUNCTION chk() RETURNS void LANGUAGE plpgsql AS \$\$ DECLARE a bigint; b bigint; BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	EXECUTE 'SELECT count(*) FROM st WHERE body @@> ''zzword''' INTO a;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE 'SELECT count(*) FROM st WHERE body LIKE ''%zzword%''' INTO b;
	IF a <> b THEN INSERT INTO diffs VALUES (a, b); END IF;
END \$\$" >/dev/null || bail "setup failed"
check "the index count is the sequential count" test "$(sql vs "SET enable_seqscan = off" "SELECT count(*) FROM st WHERE body @@> 'zzword'" | tail -1)" == "$ROWS"

printf '%s\n' 'BEGIN ISOLATION LEVEL REPEATABLE READ;' 'SELECT chk();' 'COMMIT;' >"$D/read.sql"
printf '%s\n' '\set r random(0, 999)' "DELETE FROM st WHERE id % 1000 = :r AND id <= $ROWS;" >"$D/delete.sql"
printf '%s\n' '\set r random(0, 999)' "UPDATE st SET body = body || ' u' WHERE id % 1000 = :r AND id <= $ROWS;" >"$D/update.sql"
printf '%s\n' "INSERT INTO st (body) SELECT 'zzword n' || i FROM generate_series(1, 300) i;" >"$D/insert.sql"
pgbench_bg vs 11 "$SECS" -f "$D/read.sql@60" -f "$D/delete.sql@3" -f "$D/update.sql@3" -f "$D/insert.sql@2"

n=0
: >"$D/maint.out"
while kill -0 "$PGBENCH_PID" 2>/dev/null; do
	sql vs "VACUUM st" >>"$D/maint.out" 2>&1 || echo FAILED >>"$D/maint.out"
	(( n % 2 )) || sql vs "SELECT sift.merge('st_idx', true)" >>"$D/maint.out" 2>&1 || echo FAILED >>"$D/maint.out"
	(( ++n % 5 )) || sql vs "SELECT sift.promote('st_idx')" >>"$D/maint.out" 2>&1 || echo FAILED >>"$D/maint.out"
done
wait "$PGBENCH_PID"
diag "$(grep -E "^(number of transactions actually|tps)" "$D/pgbench.log")"
diag "$n rounds of VACUUM and merge"
check "no count differed from the sequential count of its snapshot" test "$(sql vs "SELECT count(*) FROM diffs")" == 0
check "VACUUM and the merges ran without error" test "$(grep -c -E "FAILED|ERROR" "$D/maint.out")" == 0
finish
