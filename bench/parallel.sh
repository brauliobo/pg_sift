#!/usr/bin/env bash
# Speedup of the parallel SiftScan / SiftCount over the serial plans, by number of workers.
#
#   PGHOST=... PGPORT=... PGUSER=... PGDATABASE=bench bench/parallel.sh [-w "1 2 4 8"] [-r REPS] [-c REGEX]
#   (-c: only the cells whose "kind|label|query" line matches REGEX)
#
# Needs a database with the articles (bench/corpus.sh restore) in a cluster that loads sift. It builds
# the index when missing, sets the table's parallel_workers to the largest worker count (the planner's
# size rule would cap the workers of a small table) and frees the parallel costs (setup, tuple) so the
# plan shape does not depend on the cost model; the last column says whether the default costs would
# have chosen the parallel plan on their own. Every parallel result is compared with the serial one
# (md5 of the rows, in the order the plan returns them). Output: TSV with the median of REPS timed
# runs per cell (one warm-up run first) and the load average, also kept in bench/out/parallel-<time>/.
set -euo pipefail
export LC_ALL=C

WORKERS="1 2 4 8" REPS=7 ONLY=
while getopts w:r:c: opt; do
	case $opt in w) WORKERS=$OPTARG ;; r) REPS=$OPTARG ;; c) ONLY=$OPTARG ;; *) sed -n '2,13p' "$0"; exit 1 ;; esac
done
DB=${PGDATABASE:-bench}
OUT=$(dirname "$0")/out/parallel-$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
q() { psql -X -v ON_ERROR_STOP=1 -qAt -d "$DB" "$@"; }
maxw=${WORKERS##* }

q -c "CREATE EXTENSION IF NOT EXISTS sift" >/dev/null
[[ $(q -c "SELECT count(*) FROM pg_indexes WHERE tablename = 'docs' AND indexdef LIKE '%USING sift%'") -gt 0 ]] ||
	q -c "CREATE INDEX docs_fts ON docs USING sift (body)"
q -c "ALTER TABLE docs SET (parallel_workers = $maxw)" -c "VACUUM (ANALYZE) docs" >/dev/null

# kind|label|query
CELLS=(
	"count|the|SELECT count(*) FROM docs WHERE body @@> 'the'"
	"count|city or river|SELECT count(*) FROM docs WHERE body @@> 'city OR river'"
	"count|school and state|SELECT count(*) FROM docs WHERE body @@> 'school AND state'"
	"scan|city or river|SELECT id FROM docs WHERE body @@> 'city OR river' AND a_count % 7 = 0"
	"top10|city or river|SELECT id, sift.score(ctid) FROM docs WHERE body @@> 'city OR river' ORDER BY sift.score(ctid) DESC LIMIT 10"
	"top1000|city or river|SELECT id, sift.score(ctid) FROM docs WHERE body @@> 'city OR river' ORDER BY sift.score(ctid) DESC LIMIT 1000"
	"top10000|the or of or and|SELECT id, sift.score(ctid) FROM docs WHERE body @@> 'the OR of OR and' ORDER BY sift.score(ctid) DESC LIMIT 10000"
	"top10000|city or river|SELECT id, sift.score(ctid) FROM docs WHERE body @@> 'city OR river' ORDER BY sift.score(ctid) DESC LIMIT 10000"
)

settings() { # $1: workers, $2: free (0|1)
	local s="SET max_parallel_workers_per_gather = $1; SET min_parallel_index_scan_size = 0;"
	[[ $2 == 1 ]] && s+=" SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0;"
	echo "$s"
}
median_ms() { # runs the query REPS + 1 times in one session; prints the median of the last REPS
	{
		echo "$1"
		echo '\timing on'
		for ((i = 0; i <= REPS; i++)); do echo "$2;"; done
	} | psql -X -v ON_ERROR_STOP=1 -qAt -d "$DB" | sed -n 's/^Time: \([0-9.]*\) ms.*/\1/p' | tail -n "$REPS" | sort -n | sed -n "$(((REPS + 1) / 2))p"
}
# unordered scans return their rows in no particular order: compare them sorted
digest() { q -c "$1" -c "SELECT md5(string_agg(t::text, ',' $([[ $3 == scan ]] && echo 'ORDER BY t::text'))) FROM ($2) t" | tail -n 1; }
parallel_plan() { q -c "$1" -c "EXPLAIN (COSTS OFF) $2" | grep -c 'Parallel Custom Scan' || true; }

printf 'kind\tquery\tworkers\tms\tspeedup\tidentical\tparallel_plan\tauto_parallel\tload\n' | tee "$OUT/summary.tsv"
{ echo "# host: $(uptime)"; echo "# $(nproc) CPUs, $(psql -X -qAt -d "$DB" -c 'SELECT version()')"; } | tee -a "$OUT/summary.tsv" >&2
for cell in "${CELLS[@]}"; do
	[[ -z $ONLY || $cell =~ $ONLY ]] || continue
	IFS='|' read -r kind label sql <<<"$cell"
	base=$(median_ms "$(settings 0 0)" "$sql")
	ref=$(digest "$(settings 0 0)" "$sql" "$kind")
	printf '%s\t%s\t0\t%s\t1.00\tyes\t0\t-\t%s\n' "$kind" "$label" "$base" "$(cut -d' ' -f1-3 /proc/loadavg)" | tee -a "$OUT/summary.tsv"
	for w in $WORKERS; do
		ms=$(median_ms "$(settings "$w" 1)" "$sql")
		same=$([[ $(digest "$(settings "$w" 1)" "$sql" "$kind") == "$ref" ]] && echo yes || echo NO)
		printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$kind" "$label" "$w" "$ms" "$(awk "BEGIN { printf \"%.2f\", $base / $ms }")" "$same" \
			"$(parallel_plan "$(settings "$w" 1)" "$sql")" "$(parallel_plan "$(settings "$w" 0)" "$sql")" "$(cut -d' ' -f1-3 /proc/loadavg)" |
			tee -a "$OUT/summary.tsv"
	done
done
