#!/usr/bin/env bash
# Usage: bench/bench final [--size P] [--docs] [--stages LIST] [--budget MIN] [--dry-run] [--no-resume] [OPTION...]   (bench/steps/final.sh, called by bench/bench)
# The release pass: every published number, in stages, each with a time budget (bench/presets.env), within a total budget.
#   store        the store benchmark: PostgreSQL, pg_sift (sift_col, sift_table), Citus (columnar), Elasticsearch, ClickHouse; warm, cold and update tables
#   compression  the pg_sift compression variants of the column index and the codecs' sizes in every system (1M orders)
#   stats        what ClickHouse and Elasticsearch read per question
#   gap          the pg_sift profile (private cluster under a memory cap) and bench/out/BENCH-GAP.md
#   updates      the update, delete and VACUUM section of the README (store section): bulk changes at 200,000 and 1,000,000 orders, steady churn, inserts only, the reads around a churn
#   search       articles and places, every engine: warm and cold tables, charts, README.md and bench/out/BENCHMARKS.md
#   cases        the cases of bench/cases.tsv at the size of the pass, on pg_sift, PostgreSQL and pg_duckdb: tables shared by the cases that read the same one, a lane stops repeating after 20 s
#   duckdb       the ten questions of the store on pg_duckdb at 1M orders
#   generic      the generic suites (TPC-H-like, ClickBench-like, time series) on pg_duckdb and ClickHouse at 1M rows; pg_sift and PostgreSQL are in the cases stage
#   sets the set-filter benchmark (synthetic, steps/sets.sh) on the release REF (the release of SIFT_SRC=ref:REF, else this checkout)
#   scale        (named only, never in the default pass) the cases of bench/cases.tsv at 1x, 10x and 100x of 200,000 orders: bench/results/cases-scale.tsv, the block "Benchmark cases, by size"; 20 million orders at the largest point, for a host with the memory and the hours (`--budget 0`)
#   extras element rollups, table writes, BM25 sizes, leaf types, facets, build cost, GPU (steps/extras.sh), the builds they compare are tags
# The total budget is checked first (the sum of the stages that will run; exit 4 over it, --budget 0 skips the check); a stage that finished before with the same code, settings and
# host is skipped (its key is in $STORE/final-state; --no-resume runs everything). Every other option is passed on to the stages (bench/bench --help). --docs also writes bench/results,
# the charts, README.md and bench/out/BENCHMARKS.md (only at --size standard or full). --dry-run prints the plan, the estimate and the preconditions and starts nothing.
# Competitor cells (Elasticsearch, ClickHouse, Citus) come from the cache of cells unless --remeasure-all or their key changed: the stage minutes are
# those of a pass that measures everything, a pass with a warm cache takes less.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
DIE_TAG=final

ORIG=("$@")
FWD=() DRY='' STAGES='' BUDGET='' DOCS='' SIZE=$DEFAULT_PRESET ONLY='' KEEP_GOING=1 QUICK='' TABLE_LOG=''
while (( $# )); do
  case $1 in
    --dry-run) DRY=1 ;;
    --quick) QUICK=1 ;;
    --quick-table) TABLE_LOG=$2; CACHE_COLD=${CACHE_COLD:-}; shift ;;   # internal: print the table of this stage log and stop               # the quick release profile (bench/bench release-quick)
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
if [[ -n $QUICK ]]; then   # the headline cases at 300,000 and 100,000 rows, the cost of the default option at 100,000, and the store and updates stages from the cache of competitor cells
  SIZE=release; [[ -n $STAGES ]] || STAGES='headline slope cost store updates'; [[ -n $BUDGET ]] || BUDGET=60; TAG=quick
else TAG=$SIZE; fi
preset "$SIZE" || die "unknown size $SIZE ($PRESETS)"
[[ -z $DOCS || -n ${REHEARSAL:-} || $SIZE == standard || $SIZE == release || $SIZE == full ]] || die "--docs publishes numbers: they are only measured at --size standard, release or full, not $SIZE"
FWD+=(--size "$SIZE")
[[ -n $BUDGET ]] || { [[ $SIZE == full ]] && BUDGET=0 || { [[ $SIZE == standard ]] && BUDGET=125 || { [[ $SIZE == release ]] && BUDGET=120 || BUDGET=25; }; }; }
SIZES=$P_ROWS; [[ $SIZE != full ]] || SIZES=1000000,10000000
OUT=$BENCH/out/run-final-$TAG; GAPOUT=$OUT   # one directory per size: `bench resume` finds it and the stages find their runs in it
export STAGE_STATE=$STORE/final-state/$TAG STAGE_LOG=$GAPOUT/stages.tsv
ALL_STAGES="store updates compression stats gap search cases duckdb generic"
if [[ $SIZE == release || -n ${REHEARSAL:-} ]]; then ALL_STAGES="$ALL_STAGES scaling analytics sizes"; fi   # the tables of the partition and worker sweeps and of the application shapes; `sizes` is last: it runs only if it still fits the budget
[[ -n $STAGES ]] || STAGES=$ALL_STAGES
[[ $SIZE == standard || $SIZE == release || $SIZE == full || -n ${REHEARSAL:-} || ${STAGES} != "$ALL_STAGES" ]] || STAGES=${STAGES/compression /}   # the compression variants are published at 1M: smoke and quick skip them unless named

bench() { "$BENCH/bench" "$@"; }
s_store() { bench store all "${FWD[@]}" --run-id "final-$TAG-store" --sizes "$SIZES" ${DOCS:+--docs}; }
s_compression() { bench store all "${FWD[@]}" --run-id "final-$TAG-compression" --compression --sizes 1000000 --cold-runs 0 --update-sizes none ${DOCS:+--docs}; }
s_stats() { bench store stats "${FWD[@]}" --run-id "final-$TAG-stats" --sizes "$SIZES" --engines pg,sift,sift_table,es,es_cached,ch; [[ -z $DOCS ]] || cp "$BENCH"/out/latest/store-stats-*.json "$REPO/bench/results/"; }
s_gap() {
  S_SIZES=$SIZES OUT=$GAPOUT "$STEPS/gap.sh"
  [[ -z $DOCS ]] || { for f in "$GAPOUT"/gap-*.json; do cp "$f" "$REPO/bench/results/store-${f##*/}"; done
                      python3 -I "$STEPS/gap.py" render --profile "$REPO"/bench/results/store-gap-*.json --stats "$REPO"/bench/results/store-stats-*.json; }
}
s_updates() { S_REPS=${P_S_REPS:-5} S_REPS_BIG=${P_S_REPS_BIG:-3} S_CHURN_SECONDS=${P_CHURN:-20} bench store section "${FWD[@]}" --run-id "final-$TAG-updates" --sizes "${P_SECTION_SIZES:-200000,1000000}" ${DOCS:+--docs}; }
s_search() { # the slim tier of every engine, then `fuzzy` (not a class of the reduced matrix, so a column of the README table of its own) for pg_sift and Elasticsearch, merged into the tier
  local fuzzy=sift,elasticsearch
  bench all "${FWD[@]}" ${REHEARSAL:+--engines "$fuzzy"} --run-id "final-$TAG-search" ${DOCS:+--docs}   # a rehearsal builds no other competitor extension
  bench run "${FWD[@]}" --dataset articles --classes fuzzy --engines "$fuzzy" --run-id "final-$TAG-search-fuzzy" ${DOCS:+--docs --merge}
}
s_sizes() { # the store at other sizes and pg_duckdb at the big size; a rehearsal does not write them: the smoke sizes are not the ladder the charts read
  local small="final-$TAG-sizes-small" big="final-$TAG-sizes-big" scaling=$GAPOUT/store-scaling.json duck=$GAPOUT/duckdb-big.tsv
  if [[ -n $DOCS && -z ${REHEARSAL:-} ]]; then scaling=$REPO/bench/results/store-scaling.json duck=$REPO/bench/results/duckdb-10000000.tsv; fi
  bench store all "${FWD[@]}" --run-id "$small" --sizes "$P_SIZES_SMALL" --cold-runs 0 --restart-runs 0 --update-sizes none
  bench store all "${FWD[@]}" --run-id "$big" --sizes "$P_SIZES_BIG" --runs 3 --warmups 1 --cold-runs 0 --restart-runs 0 --update-sizes none
  bench store scaling "$scaling" "$BENCH/out/run-$small/store.json" "$BENCH/out/run-$big/store.json"
  bench duckdb --rows "$P_DUCKDB_BIG" --out "$duck"
}
s_duckdb() { bench duckdb ${DOCS:+--docs}; }   # the ten questions of the store on pg_duckdb at 1M orders; the 10M run is `bench duckdb --rows 10000000 --out bench/results/duckdb-10000000.tsv`
s_generic() { bench generic --rows "$P_ROWS" --runs "${P_GENERIC_RUNS:-3}" ${DOCS:+--docs}; }   # the generic suites on pg_duckdb and ClickHouse: the answers are checked against PostgreSQL's, one container at a time
cases_memory() {   # the scratch of the cases is tmpfs charged to the memory cap of the server: reserve 3x the plan with a 40G cap when 60 GB are available, with 24G when the host has 3x the plan and 8 GB, else 2x with 16G; the plan undercounts by up to 1.6x; CASES_MEM names the cap by hand
  CASES_PLAN_GB=$("$BENCH/bench" cases --plan --rows "${1:-$P_ROWS}" 2>/dev/null | sed -n 's/.*largest point needs about \([0-9]*\) GB.*/\1/p')
  CASES_AVAIL_GB=$(awk '/MemAvailable/ {printf "%d", $2 / 1048576}' /proc/meminfo)
  CASES_X=2 CASES_CAP=16G
  if (( CASES_AVAIL_GB >= 60 )) && [[ -n $CASES_PLAN_GB ]]; then CASES_X=3 CASES_CAP=40G   # the 3.6 window: 14 GB of scratch at 1,000,000 orders, 40G
  elif [[ -n $CASES_PLAN_GB ]] && (( CASES_AVAIL_GB >= CASES_PLAN_GB * 3 + 8 )); then CASES_X=3 CASES_CAP=24G; fi
  CASES_CAP=${CASES_MEM:-$CASES_CAP} CASES_RESERVE_GB=$(( ${CASES_PLAN_GB:-0} * CASES_X ))
}
s_cases() { cases_memory; CASES_MEM=$CASES_CAP bench cases ${P_CASES:+--cases "$P_CASES"} --rows "$P_ROWS" --rounds "${P_CASE_ROUNDS:-3}" ${P_CASE_RUNS:+--runs "$P_CASE_RUNS"} --lane-budget "${P_LANE_BUDGET:-20}" ${REHEARSAL:+--no-wait} ${P_ANY_PLAN:+--any-plan} ${DOCS:+--docs}; }   # the cases of bench/cases.tsv at the size of the pass
QUICK_ROWS=${QUICK_ROWS:-300000} QUICK_SLOPE_ROWS=${QUICK_SLOPE_ROWS:-100000}
quick_cases() { # ROWS FILE: the cases marked `headline` at ROWS orders, their records in FILE
  cases_memory "$1"; CASES_MEM=$CASES_CAP bench cases --headline --rows "$1" --any-plan --minstr --wait 20 --lane-budget "${P_LANE_BUDGET:-20}" --records "$2"
}
s_headline() { quick_cases "$QUICK_ROWS" "$GAPOUT/headline-$QUICK_ROWS.tsv"; }
s_slope() { quick_cases "$QUICK_SLOPE_ROWS" "$GAPOUT/headline-$QUICK_SLOPE_ROWS.tsv"; python3 -I "$STEPS/records/slope.py" "$GAPOUT/headline-$QUICK_SLOPE_ROWS.tsv" "$GAPOUT/headline-$QUICK_ROWS.tsv"; }
s_cost() { "$STEPS/cost_quick.sh" "$GAPOUT/cost-100000.tsv"; }
s_scaling() { bench scaling --rows "$P_ROWS" --reps "${P_SCALING_REPS:-7}" ${P_SCALING_PARTS:+--partitions "$P_SCALING_PARTS"} ${P_SCALING_WORKERS:+--workers "$P_SCALING_WORKERS"} ${DOCS:+--docs}; }   # partitions and parallel workers
s_analytics() { # shellcheck disable=SC2086  # $ch is one flag or nothing
  local ch=--clickhouse; [[ -x ${CH_BIN:-/usr/bin/clickhouse} ]] || { ch=''; log 'analytics: no ClickHouse binary (CH_BIN): its column of the shapes table is n/a'; }
  bench analytics run --engines heap,col,flat,ch --flat $ch ${P_ANALYTICS_ORDERS:+--orders "$P_ANALYTICS_ORDERS"} ${P_ANALYTICS_RUNS:+--runs "$P_ANALYTICS_RUNS"} ${DOCS:+--docs}; }   # the application shapes
s_scale() { CASES_MEM=${CASES_MEM:-48G} bench cases --scales 1,10,100 --rows 200000 --timeout 1800 ${SCALE_CASES:+--cases $SCALE_CASES} ${DOCS:+--docs}; }   # CASES_TMP: a disk with 30 GB; SCALE_CASES: some cases   # opt-in: not in ALL_STAGES
s_sets() { bench sets ${SIFT_SRC:+$([[ $SIFT_SRC == ref:* ]] && echo "--ref ${SIFT_SRC#ref:}")} ${DOCS:+--docs}; }
s_extras() { bench extras all ${DOCS:+--docs}; }
cache_warm() { # the cells of the servers that are not under test are in the cache (and this pass does not remeasure them): the store stage measures pg_sift alone
  [[ " ${FWD[*]} " != *" --remeasure-all "* ]] && compgen -G "$STORE_CACHE/cells/*/*.json" >/dev/null
}
minutes() { # STAGE: the minutes it takes when everything is measured
  case $1 in
    headline) echo "${P_MIN_HEADLINE:-15}" ;;   # the 26 cases at 300,000 rows: 12.7 and 17.5 min in two passes of 2026-11-03 (the host is shared)
    slope) echo "${P_MIN_SLOPE:-11}" ;;         # the same at 100,000 rows: 8.2 and 13.9 min
    cost) echo "${P_MIN_COST:-6}" ;;            # 6.0 and 6.2 min
    store) [[ -z $QUICK ]] || ! cache_warm || { echo "${P_MIN_QUICK_STORE:-9}"; return; }   # from the cache of competitor cells 9 min, with every server measured 26 (the cache is in /tmp: a reboot empties it)
           if cache_warm; then echo "$P_MIN_STORE_CACHED"   # competitor cells are in the cache
           else echo $(( P_MIN_SETUP + P_MIN_WARM + P_MIN_COLD + P_MIN_UPDATES )); fi ;;
    compression) echo "${P_MIN_COMPRESSION:-6}" ;;
    updates) [[ -n $QUICK ]] && echo "${P_MIN_QUICK_UPDATES:-5}" || echo "${P_MIN_SECTION:-30}" ;;   # --quick: 4.5 min measured
    stats) echo "${P_MIN_STATS:-3}" ;;
    gap) echo "${P_MIN_GAP:-4}" ;;
    search) echo "$P_MIN_SEARCH" ;;
    cases) echo "${P_MIN_CASES:-15}" ;;
    duckdb) echo "${P_MIN_DUCKDB:-6}" ;;
    generic) echo "${P_MIN_GENERIC:-3}" ;;
    scaling) echo "${P_MIN_SCALING:-12}" ;;
    analytics) echo "${P_MIN_ANALYTICS:-10}" ;;
    sizes) echo "${P_MIN_SIZES:-13}" ;;
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
  local total=0 withsizes=0 s m
  printf 'Release pass, size %s (bench/presets.env), stages one after another:\n' "$SIZE"
  for s in $STAGES; do
    m=$(minutes "$s")
    if why=$(carried "$s"); then printf '  %-12s carried, 0 min: %s\n' "$s" "$why"
    elif stage_done "$s" "$(key "$s")"; then printf '  %-12s skipped: finished before with the same code, settings and host (%s)\n' "$s" "$(cut -f3 "$STAGE_STATE/$s.state")"
    elif [[ $s == sizes ]]; then printf '  %-12s about %2d min, last: runs only if the minutes before it plus these fit the budget\n' "$s" "$m"; withsizes=$m
    else printf '  %-12s about %2d min%s\n' "$s" "$m" "$([[ $s == store ]] && ! cache_warm && echo " (cache cold: every server is measured; the cells go to $STORE_CACHE)")"; total=$(( total + m )); fi
  done
  [[ " $STAGES " != *" scale "* ]] || "$BENCH/bench" cases --plan --rows 200000 --scales 1,10,100 --timeout 1800 ${SCALE_CASES:+--cases $SCALE_CASES} | sed 's/^/  /' || true   # the points, the disk and the time of the scale stage
  if (( withsizes )); then printf 'Total about %d min without sizes, %d with, budget %s min.\n' "$total" "$(( total + withsizes ))" "$BUDGET"
  else printf 'Total about %d min, budget %s min.\n' "$total" "$BUDGET"; fi
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
  if [[ " $STAGES " == *" cases "* || -n $QUICK ]]; then   # the clusters of the cases are scratch on tmpfs (CASES_TMP, default /dev/shm/sift-cases, fsync off): the largest point of the plan, twice (the rehearsal peaked at 1.6x the plan: the datasets of fixed size do not shrink), and the working memory of the server on top; tmpfs is charged to the cap of the server (CASES_MEM, 16G: 8G was killed by the kernel at 6.3 GB of scratch)
    local cdir=${CASES_TMP:-/dev/shm/sift-cases} fs
    cases_memory ${QUICK:+$QUICK_ROWS}
    printf '  cases memory: plan %s GB at %s orders, %s GB of memory available -> reserve %sx = %s GB, CASES_MEM=%s\n' "${CASES_PLAN_GB:-?}" "$([[ -n $QUICK ]] && echo "$QUICK_ROWS" || echo "$P_ROWS")" "$CASES_AVAIL_GB" "$CASES_X" "$CASES_RESERVE_GB" "$CASES_CAP"
    fs=$cdir; while [[ ! -e $fs ]]; do fs=$(dirname "$fs"); done
    check "the cases fit: ${CASES_RESERVE_GB} GB free under CASES_TMP=$cdir, $(( CASES_RESERVE_GB + 8 )) GB of memory available, CASES_MEM=$CASES_CAP at least $(( CASES_RESERVE_GB + 4 )) GB" \
      bash -c '[[ $1 -gt 0 ]] && (( $(df --output=avail -BG "$2" | tail -1 | tr -dc 0-9) >= $1 && $(awk "/MemAvailable/ {printf \"%d\", \$2 / 1048576}" /proc/meminfo) >= $1 + 8 && ${3} >= $1 + 4 ))' _ "$CASES_RESERVE_GB" "$fs" "${CASES_CAP%[Gg]}"
  fi
  check "ports 56280 .. 56284 are free" bash -c "! ss -ltnH | grep -qE ':(5628[0-4])\b'"
  check "no containers of a store run exist (bench/bench store clean)" bash -c '[[ -z $(docker ps -a -q --filter label=sift-store) ]]'
  printf '  %s GB free in %s, %s GB in /tmp, %s GB of memory available\n' "$(df --output=avail -BG "$STORE" | tail -1 | tr -dc 0-9)" "$STORE" "$(df --output=avail -BG /tmp | tail -1 | tr -dc 0-9)" "$(awk '/MemAvailable/ {printf "%d", $2 / 1048576}' /proc/meminfo)"
  return $fail
}

quick_table() { # LOG: the budget table of a quick pass: planned and measured minutes of each stage of the pass (the stages inside `store` and `updates` are in the log too, and inside their stage), `cache cold` on a
  # store stage measured with no cache of competitor cells, and the total against the budget: status 4 over it
  local measured
  awk -F'\t' -v budget="$BUDGET" -v stages=" $STAGES " -v cold="$CACHE_COLD" 'BEGIN {printf "  %-12s %8s %9s\n", "stage", "planned", "measured" > "/dev/stderr"}
    index(stages, " " $1 " ") {printf "  %-12s %8d %9.1f%s\n", $1, $3, $2 / 60, ($1 == "store" && cold != "") ? "   cache cold" : "" > "/dev/stderr"; p += $3; m += $2 / 60}
    END {printf "  %-12s %8d %9.1f   (budget %d)\n", "total", p, m, budget > "/dev/stderr"}' "$1"
  measured=$(awk -F'\t' -v stages=" $STAGES " 'index(stages, " " $1 " ") {s += $2} END {printf "%d", (s + 30) / 60}' "$1")
  (( BUDGET == 0 || measured <= BUDGET )) || { log "OVER BUDGET: the quick pass took $measured min, the budget is $BUDGET min"; return 4; }
}
if [[ -n $TABLE_LOG ]]; then   # internal: the table of a stage log (the test of the table)
  quick_table "$TABLE_LOG"; exit $?
fi
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
STAGE_STATE=$STORE/final-state/$TAG   # finished stages are remembered across runs: a stage whose key is unchanged is not measured again
CACHE_COLD=''; cache_warm || CACHE_COLD=1   # before the stages run: the store stage fills the cache
T0=$SECONDS FAILED=()
for s in $STAGES; do
  rc=0
  [[ -z ${REHEARSAL:-} || $s != search ]] || git -C "$REPO" checkout -- bench/results/store.json bench/results/store-scaling.json   # the smoke store stages replaced the 1,000,000-order tiers that the charts and highlights read
  if why=$(carried "$s"); then log "stage $s: carried, $why"
  elif [[ $s == sizes ]] && (( BUDGET > 0 )) && (( (SECONDS - T0 + 30) / 60 + $(minutes sizes) > BUDGET )); then
    log "stage sizes: skipped, $(( (SECONDS - T0 + 30) / 60 )) min elapsed and about $(minutes sizes) to run do not fit the budget of $BUDGET min; bench/results/store-scaling.json and bench/results/duckdb-10000000.tsv keep the numbers of the release that measured them (the date is in each file)"
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
[[ -z $QUICK || ! -s $STAGE_LOG ]] || quick_table "$STAGE_LOG" || exit $?
(( ${#FAILED[@]} == 0 )) || { log "FAILED stages: ${FAILED[*]}"; exit 1; }
[[ -z $DOCS ]] || log "review git diff for README.md, bench/out/BENCH-GAP.md and bench/results/"
