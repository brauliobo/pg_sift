#!/usr/bin/env bash
# Usage: bench/bench v6-release [--docs] [--quick] [--stages LIST] [--remeasure-all] [--budget MIN] [--dry-run] [--keep-going | --fail-fast]   (steps/v6_release.sh, called by bench/bench)
# The post-6.0 release pass in six stages, then scripts/gen-docs.py: about 20 to 25 minutes when the rivals come from their caches, 45 to 50 when every cache is empty.
#   store-analytics  the orders benchmark at 1M and 10M orders (PostgreSQL, pg_sift, Citus, Elasticsearch, ClickHouse), the partition scaling and pg_duckdb at 10M orders
#   search-bm25      articles and places on every text engine (the slim tier, warm and cold)
#   scope-stock      the scope index against the tuned stock arms (the standard size)
#   v6-core          the 6.0 scaling ladder: memory flatness, compression, WAL, soft delete, planning, idle memory (bench/run_round.sh)
#   scaling-io       warm compute 1 to 8 workers, cold I/O io_uring / worker / sync, 1 to 16 writers (bench/bench_scaling.sh)
#   v6-cases         the focused cases of bench/cases.tsv: planned, timed against PostgreSQL and pg_duckdb, every answer compared
# The engines that are not pg_sift (Elasticsearch, ClickHouse, Citus, pg_ivm, the stock arms, the other text engines) are measured once per host, dataset, settings and adapter and then read
# from their caches (STORE cells, RIVAL_CACHE rows); --remeasure-all measures them again.
#   --docs           also write docs/results and run scripts/gen-docs.py and its check at the end (the stages pass --docs on)
#   --quick          v6-core and scaling-io at 500,000 rows on workers 1 and 4, about 90 s; --stages LIST names others (comma separated)
#   --budget MIN     the planned minutes the pass may take (default 60; 0 = no check; exit 4 over it)    --dry-run   print the plan and start nothing
#   --keep-going     (default) run on after a failed stage and list the failed ones    --fail-fast   stop at the first
set -euo pipefail
export LC_ALL=C
. "$(dirname "$0")/lib.sh"
DIE_TAG=v6-release
BENCH_CMD=$BENCH/bench

DOCS='' QUICK='' STAGES='' REMEASURE='' BUDGET=60 DRY='' KEEP_GOING=1
while (( $# )); do
  case $1 in
    --docs) DOCS=1 ;;
    --quick) QUICK=1 ;;
    --stages) STAGES=${2//,/ }; shift ;;
    --remeasure-all) REMEASURE=--remeasure-all ;;
    --budget) BUDGET=$2; shift ;;
    --dry-run) DRY=1 ;;
    --keep-going) KEEP_GOING=1 ;;
    --fail-fast) KEEP_GOING='' ;;
    -h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option $1 (--help)" ;;
  esac
  shift
done
ALL_STAGES='store-analytics search-bm25 scope-stock v6-core scaling-io v6-cases'
[[ -n $STAGES ]] || { [[ -n $QUICK ]] && STAGES='v6-core scaling-io' || STAGES=$ALL_STAGES; }
for s in $STAGES; do [[ " $ALL_STAGES " == *" $s "* ]] || die "unknown stage $s (stages: $ALL_STAGES)"; done

RIVALS=${RIVAL_CACHE:-${BENCH_STORE:-${BENCH_WORK:-/tmp/sift-bench}}/cache/rivals}
D=${DOCS:+--docs} R=$REMEASURE

stage_cmds() { # STAGE: its commands, one a line
  case $1 in
    store-analytics)
      echo "$BENCH_CMD store all --sizes 1000000,10000000 --cpus-list 3 --cold-runs 2 --update-sizes none $D $R"
      echo "$BENCH_CMD scaling --rows 1000000 $D"
      echo "$BENCH_CMD duckdb --rows 10000000 $D $R" ;;
    search-bm25) echo "$BENCH_CMD all --tier slim $D $R" ;;
    scope-stock) echo "$BENCH_CMD scope-stock --size standard $D $R" ;;
    v6-core) echo "$BENCH/run_round.sh --verify-diff $D" ;;
    scaling-io) echo "$BENCH/bench_scaling.sh $D ${QUICK:+--quick}" ;;
    v6-cases) echo "$BENCH_CMD cases $D" ;;
  esac
}

stage_minutes() { # STAGE: planned minutes, from the caches that exist
  local cached=''
  [[ -n $REMEASURE ]] || case $1 in
    store-analytics) [[ -d $STORE_CACHE/cells ]] && cached=1 ;;
    search-bm25) [[ -d $RIVALS/articles ]] && cached=1 ;;
    scope-stock) [[ -d $RIVALS/scope-stock ]] && cached=1 ;;
    *) cached=1 ;;
  esac
  case $1:${cached:-0} in
    store-analytics:1) echo 8 ;; store-analytics:0) echo 16 ;;
    search-bm25:1) echo 3 ;; search-bm25:0) echo 10 ;;
    scope-stock:1) echo 4 ;; scope-stock:0) echo 9 ;;
    v6-core:*) echo 1 ;;
    scaling-io:*) [[ -n $QUICK ]] && echo 1 || echo 4 ;;
    v6-cases:*) echo 3 ;;
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
