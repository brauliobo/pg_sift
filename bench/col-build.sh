#!/usr/bin/env bash
# CREATE INDEX time of a sift_col index, serial and with parallel workers, on a table of order lines (4 plain columns, a text column,
# a jsonb with shredded paths), in the database the PG* variables point at (pg_sift installed; scripts/pgtest.sh env prints them).
#
#   bench/col-build.sh [--rows N] [--workers "0 2 4"] [--runs N] [--mem MB]
#
# The workers are interleaved in rounds (--runs of them, default 3) so that host load hits every cell alike; prints the load average
# before and after, every time and the median per cell with the speedup over 0 workers. maintenance_work_mem must give each participant
# 32 MB for PostgreSQL to plan the workers (--mem, default 1024). The table is dropped at the end.
set -euo pipefail

ROWS=3000000 WORKERS="0 2 4" RUNS=3 MEM=1024
while (( $# )); do
	case $1 in
		--rows) ROWS=$2; shift ;;
		--workers) WORKERS=$2; shift ;;
		--runs) RUNS=$2; shift ;;
		--mem) MEM=$2; shift ;;
		*) sed -n '2,10p' "$0"; exit 1 ;;
	esac
	shift
done
sql() { psql -X -q -v ON_ERROR_STOP=1 -At "$@"; }
now() { date +%s.%N; }

sql -c "DROP TABLE IF EXISTS col_build_bench" -c "CREATE TABLE col_build_bench (id bigint, store_id int, qty int, placed timestamptz, price float8, sku text, note text, attrs jsonb)" \
	-c "INSERT INTO col_build_bench SELECT i, (i * 7919) % 5000, 1 + (i * 31) % 40, timestamptz '2024-01-01' + (i % 100000) * interval '37 seconds', ((i * 104729) % 100000) / 100.0,
		'sku-' || (i % 3000), (ARRAY['gift','late','express','fragile','promo','return'])[1 + i % 6] || '-' || (i % 977),
		jsonb_build_object('channel', (ARRAY['web','app','pos','phone'])[1 + i % 4], 'weight', (i % 500) / 10.0, 'tags', jsonb_build_array('a' || (i % 7), 'b' || (i % 13)),
		'addr', jsonb_build_object('zip', (i % 9000)::text, 'country', (ARRAY['BR','US','DE','FR'])[1 + i % 4])) FROM generate_series(1::bigint, $ROWS) i" \
	-c "VACUUM (ANALYZE, FREEZE) col_build_bench"
echo "table: $(sql -c "SELECT pg_size_pretty(pg_relation_size('col_build_bench')) || ', ' || pg_relation_size('col_build_bench') / 8192 || ' pages'"); load before: $(cut -d' ' -f1-3 /proc/loadavg)"

declare -A T
for ((r = 1; r <= RUNS; r++)); do
	for w in $WORKERS; do
		t0=$(now)
		sql -c "SET maintenance_work_mem = '${MEM}MB'" -c "SET max_parallel_maintenance_workers = $w" -c "SET min_parallel_table_scan_size = 0" \
			-c "CREATE INDEX col_build_bench_idx ON col_build_bench USING sift_col (store_id, qty, placed, price, sku, note, attrs) WITH (paths = 'attrs.channel text, attrs.weight float4, attrs.addr.country text, attrs.addr.zip text, attrs.tags[] text')" >/dev/null
		t=$(awk -v a="$(now)" -v b="$t0" 'BEGIN { printf "%.2f", a - b }')
		T[$w]+="$t "
		info=$(sql -c "SELECT segments || ' segments, ' || pg_size_pretty(pg_relation_size('col_build_bench_idx')) FROM sift.col_index_info('col_build_bench_idx')")
		sql -c "DROP INDEX col_build_bench_idx" >/dev/null
		echo "run $r workers $w: $t s  ($info)"
	done
done
echo "load after: $(cut -d' ' -f1-3 /proc/loadavg)"
base=
for w in $WORKERS; do
	med=$(printf '%s\n' ${T[$w]} | sort -n | awk '{ v[NR] = $1 } END { print v[int((NR + 1) / 2)] }')
	[[ -n $base ]] || base=$med
	echo "workers $w: median $med s  (${T[$w]% }) speedup $(awk -v a="$base" -v b="$med" 'BEGIN { printf "%.2f", a / b }')x"
done
sql -c "DROP TABLE col_build_bench"
