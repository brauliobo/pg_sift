#!/usr/bin/env bash
# A parallel GROUP BY of 50000 groups over 500000 rows (the high-cardinality case of the scaling benchmark) under a work_mem that holds every group: the records of the participants stay in the
# shared partitions (no partition goes to a file), the answer is the heap executor's, and four workers are faster than one (the leader leaves the work to the workers). At this size the
# segment of the partitions and the launch of the workers cost about as much as the scan of one of four workers, so the speedup that the scan itself would give is not reached.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 4
node_init ps 2 "autovacuum = off
max_worker_processes = 12
max_parallel_workers = 8"
node_start ps || bail "start failed"
sql ps "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"
sql ps "SET sift.test_create_format1 = off" \
	"CREATE TABLE ev AS SELECT i AS id, (i::int8 * 7919 % 50000) AS group_key, (i % 1000)::int8 AS amount, ((i * 31) % 10007)::int8 AS score FROM generate_series(1, 500000) i" \
	"CREATE INDEX ev_col ON ev USING sift_col (group_key, amount, score)" "VACUUM (FREEZE, ANALYZE) ev" >/dev/null || bail "load failed"

Q="SELECT group_key, count(*), sum(amount), min(score), max(score) FROM ev GROUP BY 1"
RUNS=$(knob SIFT_PAR_SCALE_RUNS 5 3)
SET=("SET jit = off" "SET work_mem = '64MB'" "SET parallel_leader_participation = off" "SET parallel_setup_cost = 0" "SET parallel_tuple_cost = 0"
	"SET min_parallel_table_scan_size = 0" "SET max_parallel_workers = 8" "SET sift.test_agg_par_min_rows = 0")

explained() { # WORKERS: RUNS statements of the query in one session, the log of the server's debug lines and the execution times
	local stmts=() i
	for ((i = 0; i < RUNS; i++)); do stmts+=("EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) $Q"); done
	sql ps "${SET[@]}" "SET max_parallel_workers_per_gather = $1" "SET client_min_messages = debug1" "${stmts[@]}" 2>&1
}
best_ms() { sed -n 's/^Execution Time: \([0-9.]*\) ms$/\1/p' | sort -n | head -n 1; }
answer() { # WORKERS on|off: the md5 of the rows
	sql ps "${SET[@]}" "SET max_parallel_workers_per_gather = $1" "SET sift.enable_agg = $2" \
		"SELECT md5(string_agg(x::text, ',' ORDER BY x::text)) FROM ($Q) x" | tail -n 1
}

one=$(explained 1)
four=$(explained 4)
ms1=$(best_ms <<<"$one")
ms4=$(best_ms <<<"$four")
diag "1 worker: $ms1 ms, 4 workers: $ms4 ms (best of $RUNS)"

out=$four
check "four workers run the radix strategy" test "$(grep -c 'sift agg v6 radix: [0-9]* of [0-9]* radix partitions spilled' <<<"$four")" == "$RUNS"
check "no radix partition goes to a file with four workers" test "$(grep -c 'sift agg v6 radix: 0 of [0-9]* radix partitions spilled' <<<"$four")" == "$RUNS"
check "four workers answer what the heap executor answers" test "$(answer 4 on)" == "$(answer 0 off)"
check "four workers are at least 1.2 times faster than one" awk -v a="$ms1" -v b="$ms4" 'BEGIN { exit !(b > 0 && a / b >= 1.2) }'
finish
