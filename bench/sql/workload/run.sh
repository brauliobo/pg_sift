#!/usr/bin/env bash
# The shapes of a review analytics workload against the synthetic table of test/col/workload.sql (docs/AGG.md, "Workload shapes").
#
#   bench/sql/workload/run.sh build [ROWS]       a database (WL_DB, default sift_workload) with ROWS items (default 300000) and its column index
#   bench/sql/workload/run.sh plans [FILE...]    the plan nodes of every shape (shapes/*.sql): the aggregate pushdown of the natural plan, then with
#                                                the btree, bitmap and sequential paths off (WL_FORCE=0 skips the second pass)
#   bench/sql/workload/run.sh time FILE...       run the last statement of each file (EXPLAIN stripped) with sift.enable_agg off and on, best of 3, ms
#
# The cluster is the one of scripts/pgtest.sh (SIFT_TMP, SIFT_PORT).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
eval "$(SIFT_TMP=${SIFT_TMP:-/tmp/sift-arch} "$REPO/scripts/pgtest.sh" env)"
DB=${WL_DB:-sift_workload}
sql() { psql -X -q -v ON_ERROR_STOP=0 -d "$DB" "$@"; }

case ${1:-plans} in
	build)
		psql -X -q -d postgres -c "DROP DATABASE IF EXISTS $DB" -c "CREATE DATABASE $DB"
		sql -c "CREATE EXTENSION pg_sift"
		sql -v ON_ERROR_STOP=1 -v n="${2:-300000}" -f "$REPO/test/col/workload.sql" 2>&1 | grep -v NOTICE || true
		sql -v ON_ERROR_STOP=1 -f "$HERE/index.sql" ;;
	plans)
		shift || true
		files=("$@"); (( ${#files[@]} )) || files=("$HERE"/shapes/g*.sql)
		for pass in natural forced; do
			[[ $pass == forced && ${WL_FORCE:-1} == 0 ]] && continue
			opts=(); [[ $pass == forced ]] && opts=(-c "SET enable_seqscan=off" -c "SET enable_indexscan=off" -c "SET enable_bitmapscan=off")
			for f in "${files[@]}"; do
				echo "== $pass $(basename "$f")"
				sql "${opts[@]}" -f "$f" 2>&1 | grep -E "^--|SiftAgg|Seq Scan|HashAggregate|GroupAggregate|Lookup Joins|Pushed|Fused|ERROR|Index Scan|Bitmap" || true
			done
		done ;;
	time)
		shift
		for f in "$@"; do
			q=$(sed -n '/^EXPLAIN/,$p' "$f" | sed '1s/^EXPLAIN (COSTS OFF) //' | tail -n +1)
			for mode in off on; do
				printf '%s sift.enable_agg=%s: ' "$(basename "$f")" "$mode"
				best=
				for _ in 1 2 3; do
					t=$(sql -At -c "SET sift.enable_agg=$mode" -c "SET max_parallel_workers_per_gather=0" -c '\timing on' -c "$q" 2>&1 | sed -n 's/^Time: \([0-9.]*\) ms.*/\1/p' | tail -1)
					[[ -z $best || $(echo "$t < $best" | bc -l) == 1 ]] && best=$t
				done
				echo "$best"
			done
		done ;;
esac
