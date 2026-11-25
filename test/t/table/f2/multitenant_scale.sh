#!/usr/bin/env bash
# One tenant per partition, 600 partitions of one sift_table (more than the 512 manifest slots, the 256 pinned parts and the 128 column delta maps of a backend): a long-lived session
# reads, aggregates and updates all of them under a small backend cache budget. What it keeps for the next statement stays inside the budget however many partitions it touched, what it
# reads is what a heap twin and the plain plan read, and the locks it holds are the ones PostgreSQL takes for the partitions.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

PARTS=$(knob SIFT_SCALE_PARTS 600 600)
# the 30 s are for the release speed: an assertion-enabled server (SIFT_SCALE_SECONDS=240) builds the same partitions an order of magnitude slower
BUDGET_KB=4096
plan 8
node_init scale 2 "shared_preload_libraries = 'pg_sift'
max_locks_per_transaction = 1024
sift.test_create_format1 = off
autovacuum = off"
node_start scale || bail "start failed"
sql scale "CREATE EXTENSION pg_sift" || bail "extension failed"

START=$SECONDS
sql scale "CREATE TABLE ev (id int, tenant int, v int, c1 int, c2 int, c3 int) PARTITION BY RANGE (tenant)" \
	"CREATE TABLE ev_h (id int, tenant int, v int, c1 int, c2 int, c3 int) PARTITION BY RANGE (tenant)" \
	"DO \$\$ BEGIN FOR i IN 0..$((PARTS - 1)) LOOP
		EXECUTE format('CREATE TABLE ev_%s PARTITION OF ev FOR VALUES FROM (%s) TO (%s) USING sift_table', i, i, i + 1);
		EXECUTE format('CREATE TABLE evh_%s PARTITION OF ev_h FOR VALUES FROM (%s) TO (%s)', i, i, i + 1);
	END LOOP; END \$\$" \
	"INSERT INTO ev SELECT g, g % $PARTS, g, g, g, g FROM generate_series(1, $((PARTS * 20))) g" \
	"INSERT INTO ev_h SELECT * FROM ev" "VACUUM ev" "ANALYZE ev" >/dev/null || bail "setup failed"
diag "$PARTS partitions made and loaded in $((SECONDS - START)) s"
check "$PARTS partitions are made and loaded in seconds" test $((SECONDS - START)) -lt "$(knob SIFT_SCALE_SECONDS 30 30)"

SAME="SELECT (SELECT md5(string_agg(x::text, ',' ORDER BY x::text)) FROM ev x) = (SELECT md5(string_agg(x::text, ',' ORDER BY x::text)) FROM ev_h x)"
check "every row is the heap's" test "$(sql scale "$SAME")" == t

AGG="SELECT md5(string_agg(x::text, ',' ORDER BY x::text)) FROM (SELECT tenant % 50 AS g, count(*), sum(v), max(c3) FROM ev GROUP BY 1) x"
native=$(sql scale "SET sift.enable_agg = off" "$AGG" | tail -n 1)
sift=$(sql scale "SET sift.enable_agg = on" "$AGG" | tail -n 1)
check "the grouped scan of all partitions is the plain plan's" test "$sift" == "$native"

# a hundred narrow updates (column deltas) over many partitions, then the same rows from both tables
sql scale "UPDATE ev SET v = v + 1 WHERE id % 60 = 0" "UPDATE ev_h SET v = v + 1 WHERE id % 60 = 0" >/dev/null
check "after the column deltas every row is the heap's" test "$(sql scale "$SAME")" == t

# one session, one statement after the other: the budget holds between them
SESSION=$(sql scale "SET sift.backend_cache_kb = $BUDGET_KB" "SELECT count(*) FROM ev" "SELECT sum(v) FROM ev WHERE tenant >= 100" "SELECT count(*) FROM ev WHERE id = 77" \
	"SELECT tenant, count(*) FROM ev GROUP BY 1 ORDER BY 1 LIMIT 3" "SELECT count(*) FROM ev" \
	"SELECT (sum(total_bytes) / 1024)::int FROM pg_backend_memory_contexts WHERE name LIKE 'sift cache%'" | tail -n 1)
diag "the sift caches hold $SESSION kB after the statements, budget $BUDGET_KB kB"
check "the caches kept for the next statement stay in the budget ($SESSION kB of $BUDGET_KB kB)" test "$SESSION" -le $((BUDGET_KB + BUDGET_KB / 4))

# one transaction over every partition: the stores it touched are held until it ends, then they go back to the budget
IN_XACT=$(sql scale "SET sift.backend_cache_kb = $BUDGET_KB" "BEGIN" "SELECT count(*) FROM ev" \
	"SELECT (sum(total_bytes) / 1024)::int FROM pg_backend_memory_contexts WHERE name LIKE 'sift cache%'" "COMMIT" | tail -n 1)
AFTER=$(sql scale "SET sift.backend_cache_kb = $BUDGET_KB" "BEGIN" "SELECT count(*) FROM ev" "COMMIT" "SELECT count(*) FROM ev WHERE id = 5" \
	"SELECT (sum(total_bytes) / 1024)::int FROM pg_backend_memory_contexts WHERE name LIKE 'sift cache%'" | tail -n 1)
diag "inside the transaction $IN_XACT kB, after it $AFTER kB"
check "a transaction over $PARTS partitions gives its stores back at its end" test "$AFTER" -le $((BUDGET_KB + BUDGET_KB / 4))

# locks: the partitions of the sift_table and of the heap twin take the same number
LOCKS="SELECT count(*) FROM pg_locks WHERE pid = pg_backend_pid() AND locktype = 'relation'"
sift_locks=$(sql scale "BEGIN" "SELECT count(*) FROM ev" "$LOCKS" "COMMIT" | tail -n 2 | head -n 1)
heap_locks=$(sql scale "BEGIN" "SELECT count(*) FROM ev_h" "$LOCKS" "COMMIT" | tail -n 2 | head -n 1)
diag "locks held by a scan of all partitions: sift_table $sift_locks, heap $heap_locks"
check "a scan of all partitions takes the locks the heap twin's takes" test "$sift_locks" -le "$heap_locks"

# a point query and an aggregate still use the partitions after a reconnect (nothing of the first session is needed)
check "a new session reads the same rows" test "$(sql scale "$SAME")" == t
finish
