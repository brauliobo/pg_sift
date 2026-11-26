#!/usr/bin/env bash
# Usage: bench/bench v6-release [--docs] [--quick] [--release-tier] [--gpu] [--stages LIST] [--remeasure-all] [--budget MIN] [--dry-run] [--keep-going | --fail-fast]   (steps/f2/release.sh, called by bench/bench)
# The post-6.0 release pass in balanced tiers, then scripts/gen-docs.py: about 20 minutes when the rivals come from their caches, 45 when every cache is empty.
#   store-analytics  the orders benchmark at 100k, 1M and 5M orders at 3 threads (PostgreSQL, pg_sift, Citus, Elasticsearch, ClickHouse and ClickHouse with PROJECTIONs), partition scaling at
#                    1, 16, 256 and 4096 partitions, pg_duckdb at 5M orders
#   clickbench       the 43 ClickBench queries at 5M rows (pg, sift_table, sift_col, ClickHouse with and without PROJECTIONs, DuckDB); needs the servers `bench/bench clickbench` names (PG*, --ch-url)
#   search-bm25      articles and places on every text engine (the slim tier, warm and cold)
#   scope-stock      the scope index against the tuned stock arms and ClickHouse (ch_join, ch_proj; the standard size)
#   v6-core          the 6.0 scaling ladder, 10k, 40k, 160k and 1M rows: memory flatness, compression, WAL, soft delete, planning, idle memory (bench/run_round.sh)
#   scaling-io       warm compute on 1, 2, 4 and 8 workers, cold I/O io_uring / worker / sync, 1, 4 and 16 writers at 2M rows (bench/scaling.sh)
#   v6-cases         the headline cases of bench/cases.tsv at 300,000 rows, planned, timed against PostgreSQL and pg_duckdb, every answer compared
#   cov-plan         the coverage and plan-gate queries against PostgreSQL and 5.1.0 (bench/bench cov-plan)
#   gpu              only with --gpu: the Vulkan path against the CPU one on 1M and 10M orders (bench/gpu/pg.sh, a server named by PGHOST and PGPORT)
# The engines that are not pg_sift (Elasticsearch, ClickHouse, Citus, pg_ivm, the stock arms, the other text engines) are measured once per host, dataset, settings and adapter and then read
# from their caches (STORE cells, RIVAL_CACHE rows); --remeasure-all measures them again.
#   --docs           also write docs/results and run scripts/gen-docs.py and its check at the end (the stages pass --docs on)
#   --quick          v6-core and scaling-io at 500,000 rows on workers 1 and 4, about 90 s; clickbench at 1M rows; --stages LIST names others (comma separated)
#   --release-tier   the final release tag: store, duckdb and clickbench at 10M rows as well as the balanced tiers (the rivals come from their caches: the pg_sift arms alone take about 90 s)
#   --gpu            adds the gpu stage
#   --budget MIN     the planned minutes the pass may take (default 45, 60 with --release-tier; 0 = no check; exit 4 over it)    --dry-run   print the plan and start nothing
#   --keep-going     (default) run on after a failed stage and list the failed ones    --fail-fast   stop at the first
set -euo pipefail
export LC_ALL=C
. "$(dirname "$0")/../lib.sh"
DIE_TAG=v6-release
BENCH_CMD=$BENCH/bench

DOCS='' QUICK='' STAGES='' REMEASURE='' BUDGET='' DRY='' KEEP_GOING=1 RELEASE='' GPU=''
while (( $# )); do
  case $1 in
    --docs) DOCS=1 ;;
    --quick) QUICK=1 ;;
    --release-tier) RELEASE=1 ;;
    --gpu) GPU=1 ;;
    --stages) STAGES=${2//,/ }; shift ;;
    --remeasure-all) REMEASURE=--remeasure-all ;;
    --budget) BUDGET=$2; shift ;;
    --dry-run) DRY=1 ;;
    --keep-going) KEEP_GOING=1 ;;
    --fail-fast) KEEP_GOING='' ;;
    -h|--help) sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option $1 (--help)" ;;
  esac
  shift
done
ALL_STAGES='store-analytics clickbench search-bm25 scope-stock v6-core scaling-io v6-cases cov-plan gpu'
DEFAULT_STAGES="store-analytics clickbench search-bm25 scope-stock v6-core scaling-io v6-cases cov-plan${GPU:+ gpu}"
[[ -n $STAGES ]] || { [[ -n $QUICK ]] && STAGES='v6-core scaling-io' || STAGES=$DEFAULT_STAGES; }
for s in $STAGES; do [[ " $ALL_STAGES " == *" $s "* ]] || die "unknown stage $s (stages: $ALL_STAGES)"; done
BUDGET=${BUDGET:-$([[ -n $RELEASE ]] && echo 60 || echo 45)}
STORE_SIZES=100000,1000000,5000000 CB_SCALE=5m DUCKDB_ROWS=5000000 CORE_SCALES=10000,40000,160000,1000000
[[ -z $QUICK ]] || { CB_SCALE=1m CORE_SCALES=10000,40000,160000; }
[[ -z $RELEASE ]] || { STORE_SIZES+=,10000000 CB_SCALE=10m DUCKDB_ROWS=10000000; }

RIVALS=${RIVAL_CACHE:-${BENCH_STORE:-${BENCH_WORK:-/tmp/sift-bench}}/cache/rivals}
D=${DOCS:+--docs} R=$REMEASURE

stage_cmds() { # STAGE: its commands, one a line
  case $1 in
    store-analytics)
      echo "$BENCH_CMD store all --sizes $STORE_SIZES --cpus-list 3 --cold-runs 2 --update-sizes none $D $R"
      echo "$BENCH_CMD scaling --rows 1000000 $D"
      echo "$BENCH_CMD duckdb --rows $DUCKDB_ROWS $D $R" ;;
    clickbench)
      echo "$BENCH_CMD clickbench load --scale $CB_SCALE"
      echo "$BENCH_CMD clickbench run --scale $CB_SCALE --mode both --out ${DOCS:+docs/results/clickbench.tsv}${DOCS:-bench/out/clickbench-$CB_SCALE.tsv}" ;;
    search-bm25) echo "$BENCH_CMD all --tier slim $D $R" ;;
    scope-stock) echo "$BENCH_CMD scope-stock --size standard $D $R" ;;
    v6-core) echo "$BENCH/run_round.sh --verify-diff --scales $CORE_SCALES $D" ;;
    scaling-io) echo "$BENCH/scaling.sh $D ${QUICK:+--quick}" ;;
    v6-cases) echo "$BENCH_CMD cases --headline --rows 300000 $D" ;;
    cov-plan) echo "$BENCH_CMD cov-plan --rows 100000" ;;
    gpu) echo "$BENCH/gpu/pg.sh 1000000"; echo "$BENCH/gpu/pg.sh 10000000" ;;
  esac
}

stage_minutes() { # STAGE: planned minutes of the tiers above (Spec 94), from the caches that exist
  local cached='' big=${RELEASE:+1}
  [[ -n $REMEASURE ]] || case $1 in
    store-analytics) [[ -d $STORE_CACHE/cells ]] && cached=1 ;;
    search-bm25) [[ -d $RIVALS/articles ]] && cached=1 ;;
    scope-stock) [[ -d $RIVALS/scope-stock ]] && cached=1 ;;
    *) cached=1 ;;
  esac
  case $1:${cached:-0} in
    store-analytics:1) echo $(( 4 + 2 * ${big:-0} )) ;; store-analytics:0) echo $(( 12 + 4 * ${big:-0} )) ;;
    clickbench:*) [[ -n $QUICK ]] && echo 1 || echo $(( 4 + 2 * ${big:-0} )) ;;
    search-bm25:1) echo 2 ;; search-bm25:0) echo 9 ;;
    scope-stock:1) echo 2 ;; scope-stock:0) echo 7 ;;
    v6-core:*) [[ -n $QUICK ]] && echo 1 || echo 2 ;;
    scaling-io:*) [[ -n $QUICK ]] && echo 1 || echo 4 ;;
    v6-cases:*) echo 3 ;;
    cov-plan:*) echo 2 ;;
    gpu:*) echo 2 ;;
  esac
}

planned=0
printf '%-16s %8s  %s\n' stage minutes command
for s in $STAGES; do
  m=$(stage_minutes "$s"); planned=$(( planned + m ))
  c=$(stage_cmds "$s" | head -1 | tr -s ' ')
  printf '%-16s %8s  %s\n' "$s" "$m" "${c//$REPO\//}"
done
printf '%-16s %8s  budget %s min%s\n' total "$planned" "$BUDGET" "${DOCS:+, then scripts/gen-docs.py and --check}"
(( BUDGET == 0 || planned <= BUDGET )) || { echo "v6-release: planned $planned min is over the budget of $BUDGET min" >&2; exit 4; }
[[ -z $DRY ]] || exit 0

t_all=$SECONDS failed=()
for s in $STAGES; do
  t0=$SECONDS
  echo "=== v6-release: $s"
  while IFS= read -r c; do
    # shellcheck disable=SC2086  # the words of a command
    $c || { failed+=("$s"); [[ -z $KEEP_GOING ]] && break 2; break; }
  done < <(stage_cmds "$s")
  echo "=== v6-release: $s done in $(( (SECONDS - t0 + 30) / 60 )) min"
done
if [[ -n $DOCS ]]; then
  python3 -B -I "$REPO/scripts/gen-docs.py"
  python3 -B -I "$REPO/scripts/gen-docs.py" --check
fi
echo "v6-release: $(( (SECONDS - t_all + 30) / 60 )) min measured against $planned planned${failed[*]:+; failed stages: ${failed[*]}}"
[[ ${#failed[@]} == 0 ]] || exit 1
