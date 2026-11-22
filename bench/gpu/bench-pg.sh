#!/usr/bin/env bash
# The 6.0 Vulkan path against the CPU one, inside PostgreSQL, on the orders table of bench/sql/store (the integration tree).
#
#   bench/gpu/bench-pg.sh ROWS [REPS]       ROWS: 1000000, 10000000, 50000000; REPS: runs after the first one (default 7)
#
# Uses the server the PG* variables name; builds database sift_gpu_<ROWS> once (the table, one sift_col index over the leaves the queries read,
# VACUUM FREEZE). One psql session per query and mode:
#   cpu1  sift.gpu = off, no parallel workers
#   cpu4  sift.gpu = off, 4 parallel workers
#   gpu   sift.gpu = on:   first = the worker opens the device and the cache is empty (the chunks cross PCIe), rest = the chunks are resident
#   auto  sift.gpu = auto: the cost model decides; the GPU: line of EXPLAIN says what it chose and why
# Prints a TSV: query, mode, first_ms, median_of_rest_ms, gpu (the GPU: line of EXPLAIN ANALYZE). MODES="cpu1 cpu4" runs the CPU rows only.
# The numbers are Execution Time of EXPLAIN (ANALYZE, TIMING OFF). The integration tree's bench/sql/store/gen.sql makes orders_src.
set -euo pipefail
cd "${SIFT_TREE:-$(dirname "$0")/../..}"

N=${1:?usage: bench-pg.sh ROWS [REPS]}
REPS=${2:-7}
MODES=${MODES:-"cpu1 cpu4 gpu auto"}
DB=sift_gpu_$N
P="psql -X -q -At -v ON_ERROR_STOP=1 -d $DB"

QUERIES=(
	"ungrouped|SELECT count(*), sum(amount), min(quantity), max(quantity) FROM orders WHERE quantity > 2"
	"dense-group|SELECT country, count(*), sum(amount) FROM orders GROUP BY country"
	"hash-group|SELECT customer_id, category, count(*), sum(amount), max(quantity) FROM orders GROUP BY 1, 2"
	"expression|SELECT count(*), sum(amount * quantity - amount / (quantity + 1)) FROM orders"
	"top-100|SELECT id, amount FROM orders ORDER BY amount DESC LIMIT 100"
)

if ! psql -X -At -d postgres -c "SELECT 1 FROM pg_database WHERE datname = '$DB'" | grep 1 >/dev/null; then
	createdb "$DB"
	$P -c "CREATE EXTENSION pg_sift"
	$P -v n="$N" -f bench/sql/store/gen.sql
	$P <<-'SQL'
	CREATE TABLE orders AS SELECT id, customer_id, country, category, status, amount, quantity, created_at FROM orders_src;
	DROP TABLE orders_src;
	CREATE INDEX orders_col ON orders USING sift_col (id, customer_id, country, category, status, amount, quantity, created_at);
	CREATE TABLE warm AS SELECT i AS id, i % 7 AS g, i % 1000 AS v FROM generate_series(1, 200000) i;
	CREATE INDEX warm_col ON warm USING sift_col (id, g, v);
	VACUUM (FREEZE, ANALYZE) orders;
	VACUUM (FREEZE, ANALYZE) warm;
	SQL
fi

median() { sort -n | awk '{a[NR] = $1} END {print a[int((NR + 1) / 2)]}'; }
out=$(mktemp "${TMPDIR:-/tmp}/sift-vk-bench.XXXXXX")
trap 'rm -f "$out"' EXIT

for q in "${QUERIES[@]}"; do
	name=${q%%|*}
	sql=${q#*|}
	for mode in $MODES; do
		{
			echo "SET client_min_messages = warning; SET sift.gpu_min_rows = 1;"
			case $mode in
			cpu1) echo "SET sift.gpu = off; SET max_parallel_workers_per_gather = 0;" ;;
			cpu4) echo "SET sift.gpu = off; SET max_parallel_workers_per_gather = 4;" ;;
			gpu) echo "SET max_parallel_workers_per_gather = 0; SET sift.gpu = on; SELECT count(*), sum(v) FROM warm WHERE v > 5;" ;;
			auto) echo "SET max_parallel_workers_per_gather = 4; SET sift.gpu = auto; SET sift.gpu_min_rows = DEFAULT;" ;;
			esac
			for _ in $(seq 0 "$REPS"); do echo "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, BUFFERS OFF) $sql;"; done
		} | $P >"$out"
		first=$(grep -m1 'Execution Time' "$out" | awk '{print $3}')
		rest=$(grep 'Execution Time' "$out" | tail -n +2 | awk '{print $3}' | median)
		gpu=$(grep -m1 'GPU:' "$out" | sed 's/^ *GPU: //' || true)
		printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$mode" "$first" "$rest" "$gpu"
	done
done
