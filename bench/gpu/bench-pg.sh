#!/usr/bin/env bash
# The Vulkan aggregate path (docs/GPU.md) against the CPU one, inside PostgreSQL, on the orders table of bench/sql/store.
#
#   bench/gpu/bench-pg.sh ROWS [REPS]       ROWS: 1000000, 10000000; REPS: runs after the first one (default 7)
#
# Uses the server the PG* variables name (eval "$(scripts/pgtest.sh env)"); builds database sift_gpu_<ROWS> once (the table, one sift_col
# index over the leaves the queries read, VACUUM FREEZE). One psql session per query and mode:
#   cpu   sift.gpu = off, no parallel workers: one core
#   gpu   sift.gpu = on:  first = the device is open and the cache is empty (the chunks cross PCIe), rest = the chunks are resident
# Prints a TSV: query, mode, first_ms, median_of_rest_ms, gpu (the GPU: line of EXPLAIN ANALYZE, units used / units).
# MODES="cpu" runs the CPU rows only (a build without Vulkan). The numbers are Execution Time of EXPLAIN (ANALYZE, TIMING OFF).
set -euo pipefail
cd "$(dirname "$0")/../.."

N=${1:?usage: bench-pg.sh ROWS [REPS]}
REPS=${2:-7}
MODES=${MODES:-"cpu gpu"}
DB=sift_gpu_$N
P="psql -X -q -At -v ON_ERROR_STOP=1 -d $DB"

QUERIES=(
	"q7|SELECT count(*) FROM orders WHERE amount > 100 AND created_at >= '2025-12-02'"
	"q8|SELECT count(*) FROM orders WHERE quantity >= 4 AND amount < 50"
	"q9|SELECT count(*), sum(amount), sum(quantity) FROM orders WHERE amount > 20 AND quantity >= 2"
	"g1|SELECT country, count(*), sum(amount) FROM orders WHERE quantity >= 2 GROUP BY country"
	"g2|SELECT category, count(*), sum(quantity) FROM orders WHERE amount > 100 GROUP BY category"
	"g3|SELECT date_trunc('month', created_at)::date, sum(amount) FROM orders WHERE quantity >= 2 GROUP BY 1"
)

if ! psql -X -At -d postgres -c "SELECT 1 FROM pg_database WHERE datname = '$DB'" | grep -q 1; then
	createdb "$DB"
	$P -c "CREATE EXTENSION pg_sift"
	$P -v n="$N" -f bench/sql/store/gen.sql
	$P <<-'EOF'
	CREATE TABLE orders AS SELECT id, customer_id, country, category, status, amount, quantity, created_at FROM orders_src;
	DROP TABLE orders_src;
	CREATE INDEX orders_col ON orders USING sift_col (customer_id, country, category, status, amount, quantity, created_at,
	                                                  (date_trunc('month', created_at)::date));
	CREATE TABLE warm AS SELECT i AS id, i % 7 AS g, i % 1000 AS v FROM generate_series(1, 200000) i;
	CREATE INDEX warm_col ON warm USING sift_col (id, g, v);
	VACUUM (FREEZE, ANALYZE) orders;
	VACUUM (FREEZE, ANALYZE) warm;
	EOF
fi

median() { sort -n | awk '{a[NR] = $1} END {print a[int((NR + 1) / 2)]}'; }

for q in "${QUERIES[@]}"; do
	name=${q%%|*}
	sql=${q#*|}
	for mode in $MODES; do
		{
			echo "SET max_parallel_workers_per_gather = 0; SET client_min_messages = warning; SET sift.gpu_min_rows = 1;"
			if [ "$mode" = gpu ]; then
				echo "SET sift.gpu = on; SELECT count(*), sum(v) FROM warm WHERE v > 5;"	# opens the device
			else
				echo "SET sift.gpu = off;"
			fi
			for _ in $(seq 0 "$REPS"); do echo "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, BUFFERS OFF) $sql;"; done
		} | $P >"/tmp/sift-gpu-bench-$$.out"
		first=$(grep -m1 'Execution Time' "/tmp/sift-gpu-bench-$$.out" | awk '{print $3}')
		rest=$(grep 'Execution Time' "/tmp/sift-gpu-bench-$$.out" | tail -n +2 | awk '{print $3}' | median)
		gpu=$(grep -m1 'GPU:' "/tmp/sift-gpu-bench-$$.out" | sed 's/^ *GPU: //' || true)
		printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$mode" "$first" "$rest" "$gpu"
	done
done
rm -f "/tmp/sift-gpu-bench-$$.out"
