#!/usr/bin/env bash
# Usage: bench/bench final [--size P] [--docs] [--stages LIST] [--budget MIN] [--dry-run] [--no-resume] [OPTION...]   (bench/steps/final.sh, called by bench/bench)
# The release pass: every published number, in stages, each with a time budget (bench/presets.env), within a total budget.
#   store        the store benchmark: PostgreSQL, pg_sift (sift_col, sift_table), Citus (columnar), Elasticsearch, ClickHouse; warm, cold and update tables
#   compression  the pg_sift compression variants of the column index and the codecs' sizes in every system (1M orders)
#   stats        what ClickHouse and Elasticsearch read per question
#   gap          the pg_sift profile (private cluster under a memory cap) and docs/BENCH-GAP.md
#   updates      the update, delete and VACUUM section of the README (store section): bulk changes at 200,000 and 1,000,000 orders, steady churn, inserts only, the reads around a churn
#   search       articles and places, every engine: warm and cold tables, charts, README.md and docs/BENCHMARKS.md
#   cases        the cases of bench/cases.tsv at the size of the pass, on pg_sift, PostgreSQL and pg_duckdb: tables shared by the cases that read the same one, a lane stops repeating after 20 s
#   duckdb       the ten questions of the store on pg_duckdb at 1M orders
#   generic      the generic suites (TPC-H-like, ClickBench-like, time series) on pg_duckdb and ClickHouse at 1M rows; pg_sift and PostgreSQL are in the cases stage
#   sets the set-filter benchmark (synthetic, steps/sets.sh) on the release REF (the release of SIFT_SRC=ref:REF, else this checkout)
#   scale        (named only, never in the default pass) the cases of bench/cases.tsv at 1x, 10x and 100x of 200,000 orders: bench/results/cases-scale.tsv, the block "Benchmark cases, by size"; 20 million orders at the largest point, for a host with the memory and the hours (`--budget 0`)
#   extras element rollups, table writes, BM25 sizes, leaf types, facets, build cost, GPU (steps/extras.sh), the builds they compare are tags
# The total budget is checked first (the sum of the stages that will run; exit 4 over it, --budget 0 skips the check); a stage that finished before with the same code, settings and
# host is skipped (its key is in $STORE/final-state; --no-resume runs everything). Every other option is passed on to the stages (bench/bench --help). --docs also writes bench/results,
# the charts, README.md and docs/BENCHMARKS.md (only at --size standard or full). --dry-run prints the plan, the estimate and the preconditions and starts nothing.
# Competitor cells (Elasticsearch, ClickHouse, Citus) come from the cache of cells unless --remeasure-all or their key changed: the stage minutes are
# those of a pass that measures everything, a pass with a warm cache takes less.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
DIE_TAG=final

ORIG=("$@")
FWD=() DRY='' STAGES='' BUDGET='' DOCS='' SIZE=$DEFAULT_PRESET ONLY='' KEEP_GOING=1
while (( $# )); do
  case $1 in
    --dry-run) DRY=1 ;;
    --stage-only) ONLY=$2; shift ;;   # internal: the stage in a process of its own
    --fail-fast) KEEP_GOING='' ;;
    --keep-going) KEEP_GOING=1 ;;
    --run-id) shift ;;                # the runs of the stages are named after the stage
    --stages) STAGES=${2//,/ }; shift ;;
    --budget) BUDGET=$2; shift ;;
    --no-resume) export NO_RESUME=1 ;;
    --no-carry|--remeasure-all) export NO_CARRY=1; [[ $1 == --no-carry ]] || FWD+=("$1") ;;
    --docs) DOCS=1 ;;
    --size) SIZE=$2; shift ;;
    *) FWD+=("$1") ;;
  esac
  shift
done
preset "$SIZE" || die "unknown size $SIZE ($PRESETS)"
[[ -z $DOCS || -n ${REHEARSAL:-} || $SIZE == standard || $SIZE == release || $SIZE == full ]] || die "--docs publishes numbers: they are only measured at --size standard, release or full, not $SIZE"
FWD+=(--size "$SIZE")
[[ -n $BUDGET ]] || { [[ $SIZE == full ]] && BUDGET=0 || { [[ $SIZE == standard ]] && BUDGET=125 || { [[ $SIZE == release ]] && BUDGET=120 || BUDGET=25; }; }; }
SIZES=$P_ROWS; [[ $SIZE != full ]] || SIZES=1000000,10000000
OUT=$BENCH/out/run-final-$SIZE; GAPOUT=$OUT   # one directory per size: `bench resume` finds it and the stages find their runs in it
export STAGE_STATE=$STORE/final-state/$SIZE STAGE_LOG=$GAPOUT/stages.tsv
ALL_STAGES="store updates compression stats gap search cases duckdb generic"
if [[ $SIZE == release || -n ${REHEARSAL:-} ]]; then ALL_STAGES="$ALL_STAGES scaling analytics"; fi   # the tables of the partition and worker sweeps and of the application shapes
[[ -n $STAGES ]] || STAGES=$ALL_STAGES
[[ $SIZE == standard || $SIZE == release || $SIZE == full || -n ${REHEARSAL:-} || ${STAGES} != "$ALL_STAGES" ]] || STAGES=${STAGES/compression /}   # the compression variants are published at 1M: smoke and quick skip them unless named

bench() { "$BENCH/bench" "$@"; }
s_store() { bench store all "${FWD[@]}" --run-id "final-$SIZE-store" --sizes "$SIZES" ${DOCS:+--docs}; }
s_compression() { bench store all "${FWD[@]}" --run-id "final-$SIZE-compression" --compression --sizes 1000000 --cold-runs 0 --update-sizes none ${DOCS:+--docs}; }
s_stats() { bench store stats "${FWD[@]}" --run-id "final-$SIZE-stats" --sizes "$SIZES" --engines pg,sift,sift_table,es,es_cached,ch; [[ -z $DOCS ]] || cp "$BENCH"/out/latest/store-stats-*.json "$REPO/bench/results/"; }
s_gap() {
  S_SIZES=$SIZES OUT=$GAPOUT "$STEPS/gap.sh"
  [[ -z $DOCS ]] || { for f in "$GAPOUT"/gap-*.json; do cp "$f" "$REPO/bench/results/store-${f##*/}"; done
                      python3 -I "$STEPS/gap.py" render --profile "$REPO"/bench/results/store-gap-*.json --stats "$REPO"/bench/results/store-stats-*.json; }
}
s_updates() { S_REPS=${P_S_REPS:-5} S_REPS_BIG=${P_S_REPS_BIG:-3} S_CHURN_SECONDS=${P_CHURN:-20} bench store section "${FWD[@]}" --run-id "final-$SIZE-updates" --sizes "${P_SECTION_SIZES:-200000,1000000}" ${DOCS:+--docs}; }
s_search() { # the slim tier of every engine, then `fuzzy` (not a class of the reduced matrix, so a column of the README table of its own) for pg_sift and Elasticsearch, merged into the tier
  local fuzzy=sift,elasticsearch
  bench all "${FWD[@]}" ${REHEARSAL:+--engines "$fuzzy"} --run-id "final-$SIZE-search" ${DOCS:+--docs}   # a rehearsal builds no other competitor extension
  bench run "${FWD[@]}" --dataset articles --classes fuzzy --engines "$fuzzy" --run-id "final-$SIZE-search-fuzzy" ${DOCS:+--docs --merge}
}
s_duckdb() { bench duckdb ${DOCS:+--docs}; }   # the ten questions of the store on pg_duckdb at 1M orders; the 10M run is `bench duckdb --rows 10000000 --out bench/results/duckdb-10000000.tsv`
s_generic() { bench generic --rows "$P_ROWS" --runs "${P_GENERIC_RUNS:-3}" ${DOCS:+--docs}; }   # the generic suites on pg_duckdb and ClickHouse: the answers are checked against PostgreSQL's, one container at a time
cases_memory() {   # the scratch of the cases is tmpfs charged to the memory cap of the server: reserve 3x the plan (cap 24G) when the host has 3x the plan and 8 GB of memory available, else 2x (cap 16G); the plan undercounts by up to 1.6x; CASES_MEM names the cap by hand
  CASES_PLAN_GB=$("$BENCH/bench" cases --plan --rows "$P_ROWS" 2>/dev/null | sed -n 's/.*largest point needs about \([0-9]*\) GB.*/\1/p')
  CASES_AVAIL_GB=$(awk '/MemAvailable/ {printf "%d", $2 / 1048576}' /proc/meminfo)
  CASES_X=2 CASES_CAP=16G
  if [[ -n $CASES_PLAN_GB ]] && (( CASES_AVAIL_GB >= CASES_PLAN_GB * 3 + 8 )); then CASES_X=3 CASES_CAP=24G; fi
  CASES_CAP=${CASES_MEM:-$CASES_CAP} CASES_RESERVE_GB=$(( ${CASES_PLAN_GB:-0} * CASES_X ))
}
s_cases() { cases_memory; CASES_MEM=$CASES_CAP bench cases ${P_CASES:+--cases "$P_CASES"} --rows "$P_ROWS" --rounds "${P_CASE_ROUNDS:-3}" ${P_CASE_RUNS:+--runs "$P_CASE_RUNS"} --lane-budget "${P_LANE_BUDGET:-20}" ${REHEARSAL:+--no-wait} ${P_ANY_PLAN:+--any-plan} ${DOCS:+--docs}; }   # the cases of bench/cases.tsv at the size of the pass
s_scaling() { bench scaling --rows "$P_ROWS" --reps "${P_SCALING_REPS:-7}" ${P_SCALING_PARTS:+--partitions "$P_SCALING_PARTS"} ${P_SCALING_WORKERS:+--workers "$P_SCALING_WORKERS"} ${DOCS:+--docs}; }   # partitions and parallel workers
s_analytics() { # shellcheck disable=SC2086  # $ch is one flag or nothing
  local ch=--clickhouse; [[ -x ${CH_BIN:-/usr/bin/clickhouse} ]] || { ch=''; log 'analytics: no ClickHouse binary (CH_BIN): its column of the shapes table is n/a'; }
  bench analytics run --engines heap,col,flat,ch --flat $ch ${P_ANALYTICS_ORDERS:+--orders "$P_ANALYTICS_ORDERS"} ${P_ANALYTICS_RUNS:+--runs "$P_ANALYTICS_RUNS"} ${DOCS:+--docs}; }   # the application shapes
s_scale() { CASES_MEM=${CASES_MEM:-48G} bench cases --scales 1,10,100 --rows 200000 --timeout 1800 ${SCALE_CASES:+--cases $SCALE_CASES} ${DOCS:+--docs}; }   # CASES_TMP: a disk with 30 GB; SCALE_CASES: some cases   # opt-in: not in ALL_STAGES
s_sets() { bench sets ${SIFT_SRC:+$([[ $SIFT_SRC == ref:* ]] && echo "--ref ${SIFT_SRC#ref:}")} ${DOCS:+--docs}; }
s_extras() { bench extras all ${DOCS:+--docs}; }
minutes() { # STAGE: the minutes it takes when everything is measured
  case $1 in
    store) if [[ " ${FWD[*]} " != *" --remeasure-all "* ]] && compgen -G "${S_CACHE_DIR:-$STORE/store-cache}/cells/*/*.json" >/dev/null; then echo "$P_MIN_STORE_CACHED"   # competitor cells are in the cache
           else echo $(( P_MIN_SETUP + P_MIN_WARM + P_MIN_COLD + P_MIN_UPDATES )); fi ;;
    compression) echo "${P_MIN_COMPRESSION:-6}" ;;
    updates) echo "${P_MIN_SECTION:-30}" ;;
    stats) echo "${P_MIN_STATS:-3}" ;;
    gap) echo "${P_MIN_GAP:-4}" ;;
    search) echo "$P_MIN_SEARCH" ;;
    cases) echo "${P_MIN_CASES:-15}" ;;
    duckdb) echo "${P_MIN_DUCKDB:-6}" ;;
    generic) echo "${P_MIN_GENERIC:-3}" ;;
    scaling) echo "${P_MIN_SCALING:-12}" ;;
    analytics) echo "${P_MIN_ANALYTICS:-10}" ;;
    scale) echo "${P_MIN_SCALE:-430}" ;;
    sets) echo "${P_MIN_SETS:-25}" ;;
    extras) echo "${P_MIN_EXTRAS:-100}" ;;
  esac
}
key() { python3 -I "$STEPS/benchlib.py" key "$(code_key)" "$(fingerprint)" "$1" "$SIZE" "${FWD[*]}" "$DOCS" "$SIZES"; }

carried() { # STAGE: the stage whose committed results stand in for a new run, with the line that says why. Only `generic` (the other engines: it does not run pg_sift, so a commit that changes pg_sift leaves its inputs alone)
  [[ -z ${NO_CARRY:-} && $1 == generic && $SIZE == release ]] || return 1
  python3 -I "$STEPS/generic.py" key --rows "$P_ROWS" --runs "${P_GENERIC_RUNS:-3}" 2>/dev/null
}

plan() { # the stages that will run, their minutes, the total against the budget
  local total=0 s m
  printf 'Release pass, size %s (bench/presets.env), stages one after another:\n' "$SIZE"
  for s in $STAGES; do
    m=$(minutes "$s")
    if why=$(carried "$s"); then printf '  %-12s carried, 0 min: %s\n' "$s" "$why"
    elif stage_done "$s" "$(key "$s")"; then printf '  %-12s skipped: finished before with the same code, settings and host (%s)\n' "$s" "$(cut -f3 "$STAGE_STATE/$s.state")"
    else printf '  %-12s about %2d min\n' "$s" "$m"; total=$(( total + m )); fi
  done
  [[ " $STAGES " != *" scale "* ]] || "$BENCH/bench" cases --plan --rows 200000 --scales 1,10,100 --timeout 1800 ${SCALE_CASES:+--cases $SCALE_CASES} | sed 's/^/  /' || true   # the points, the disk and the time of the scale stage
  printf 'Total about %d min, budget %s min.\n' "$total" "$BUDGET"
  TOTAL=$total
}
check() { # DESCRIPTION COMMAND...
  local what=$1; shift
  if "$@" >/dev/null 2>&1; then printf '  ok       %s\n' "$what"; else printf '  MISSING  %s\n' "$what"; fail=1; fi
}
preconditions() {
  fail=0
  printf 'Preconditions:\n'
  check "docker works" docker info
  check "systemd-run --user scope works (the memory cap of the pg_sift profile)" systemd-run --user --scope -q -p MemoryMax=1G true
  check "psql, python3, flock, ss, perf, pgbench, pg_config" bash -c 'for c in psql python3 flock ss perf pgbench pg_config; do command -v $c || exit 1; done'
  [[ $SIZE != release || -n ${REHEARSAL:-} ]] || check "a ClickHouse binary for the application shapes (CH_BIN, default /usr/bin/clickhouse)" test -x "${CH_BIN:-/usr/bin/clickhouse}"
  if [[ " $STAGES " == *" cases "* ]]; then   # the clusters of the cases are scratch on tmpfs (CASES_TMP, default /dev/shm/sift-cases, fsync off): the largest point of the plan, twice (the rehearsal peaked at 1.6x the plan: the datasets of fixed size do not shrink), and the working memory of the server on top; tmpfs is charged to the cap of the server (CASES_MEM, 16G: 8G was killed by the kernel at 6.3 GB of scratch)
    local cdir=${CASES_TMP:-/dev/shm/sift-cases} fs
    cases_memory
    printf '  cases memory: plan %s GB at %s orders, %s GB of memory available -> reserve %sx = %s GB, CASES_MEM=%s\n' "${CASES_PLAN_GB:-?}" "$P_ROWS" "$CASES_AVAIL_GB" "$CASES_X" "$CASES_RESERVE_GB" "$CASES_CAP"
    fs=$cdir; while [[ ! -e $fs ]]; do fs=$(dirname "$fs"); done
    check "the cases fit: ${CASES_RESERVE_GB} GB free under CASES_TMP=$cdir, $(( CASES_RESERVE_GB + 8 )) GB of memory available, CASES_MEM=$CASES_CAP at least $(( CASES_RESERVE_GB + 4 )) GB" \
      bash -c '[[ $1 -gt 0 ]] && (( $(df --output=avail -BG "$2" | tail -1 | tr -dc 0-9) >= $1 && $(awk "/MemAvailable/ {printf \"%d\", \$2 / 1048576}" /proc/meminfo) >= $1 + 8 && ${3} >= $1 + 4 ))' _ "$CASES_RESERVE_GB" "$fs" "${CASES_CAP%[Gg]}"
  fi
  check "ports 56280 .. 56284 are free" bash -c "! ss -ltnH | grep -qE ':(5628[0-4])\b'"
  check "no containers of a store run exist (bench/bench store clean)" bash -c '[[ -z $(docker ps -a -q --filter label=sift-store) ]]'
  printf '  %s GB free in %s, %s GB in /tmp, %s GB of memory available\n' "$(df --output=avail -BG "$STORE" | tail -1 | tr -dc 0-9)" "$STORE" "$(df --output=avail -BG /tmp | tail -1 | tr -dc 0-9)" "$(awk '/MemAvailable/ {printf "%d", $2 / 1048576}' /proc/meminfo)"
  return $fail
}

if [[ -n $ONLY ]]; then   # one stage, in this process (the pass starts one per stage)
  mkdir -p "$GAPOUT" "$STAGE_STATE"
  trap 'run_exit $?' EXIT
  stage_run "$ONLY" "$(key "$ONLY")" "$(minutes "$ONLY")" "s_$ONLY"
  exit 0
fi
run_reclaim
plan
preconditions || { [[ -n $DRY ]] || die 'preconditions missing (bench/bench final --dry-run)'; }
[[ -z $DRY ]] || { echo 'Dry run: nothing was started.'; exit 0; }
(( BUDGET == 0 || TOTAL <= BUDGET )) || die "the stages take about $TOTAL min, over the budget of $BUDGET min: --budget MIN, --stages LIST or a smaller --size"

mkdir -p "$GAPOUT" "$STAGE_STATE"
perf_hold
RUN_DIR=$GAPOUT/.run
run_begin final "${ORIG[@]}"
STAGE_STATE=$STORE/final-state/$SIZE   # finished stages are remembered across runs: a stage whose key is unchanged is not measured again
T0=$SECONDS FAILED=()
for s in $STAGES; do
  rc=0
  [[ -z ${REHEARSAL:-} || $s != search ]] || git -C "$REPO" checkout -- bench/results/store.json bench/results/store-scaling.json   # the smoke store stages replaced the 1,000,000-order tiers that the charts and highlights read
  if why=$(carried "$s"); then log "stage $s: carried, $why"
  elif ! stage_done "$s" "$(key "$s")"; then
    "$0" --stage-only "$s" "${ORIG[@]}" &
    STAGE_PID=$!
    wait "$STAGE_PID" || rc=$?
    STAGE_PID=''
  else log "stage $s: finished before ($(cut -f3 "$STAGE_STATE/$s.state")), resumed"; fi
  (( rc )) || continue
  (( rc != EX_RESUMABLE )) || exit "$rc"
  FAILED+=("$s"); log "stage $s FAILED (exit $rc); its error is above, \`bench/bench resume\` runs it again"
  [[ -n $KEEP_GOING ]] || break
done
bench store clean >/dev/null 2>&1 || true
log "final: $(( (SECONDS - T0 + 30) / 60 )) min (budget $BUDGET); stages:"
[[ ! -s $STAGE_LOG ]] || awk -F'\t' '{printf "  %-12s %5.1f min (budget %s)\n", $1, $2 / 60, $3}' "$STAGE_LOG" >&2
(( ${#FAILED[@]} == 0 )) || { log "FAILED stages: ${FAILED[*]}"; exit 1; }
[[ -z $DOCS ]] || log "review git diff for README.md, docs/BENCH-GAP.md and bench/results/"
