#!/usr/bin/env bash
# Usage: bench/bench final [--size P] [--docs] [--stages LIST] [--budget MIN] [--dry-run] [--no-resume] [OPTION...]   (bench/steps/final.sh, called by bench/bench)
# The release pass: every published number, in stages, each with a time budget (bench/presets.env), within a total budget (default 25 min at --size standard).
#   store        the store benchmark: PostgreSQL, pg_sift (sift_col, sift_table), Citus (columnar), Elasticsearch, ClickHouse; warm, cold and update tables
#   compression  the pg_sift compression variants of the column index and the codecs' sizes in every system (1M orders)
#   stats        what ClickHouse and Elasticsearch read per question
#   gap          the pg_sift profile (private cluster under a memory cap) and docs/BENCH-GAP.md
#   search       articles and places, every engine: warm and cold tables, charts, README.md and docs/BENCHMARKS.md
# The total budget is checked first (the sum of the stages that will run; exit 4 over it, --budget 0 skips the check); a stage that finished before with the same code, settings and
# host is skipped (its key is in $STORE/final-state; --no-resume runs everything). Every other option is passed on to the stages (bench/bench --help). --docs also writes docs/results,
# the charts, README.md and docs/BENCHMARKS.md (only at --size standard or full). --dry-run prints the plan, the estimate and the preconditions and starts nothing.
# Competitor cells (Elasticsearch, ClickHouse, Citus) come from the cache of cells unless --remeasure-all or their key changed (docs/BENCHMARKS.md, "Cached cells"): the stage minutes are
# those of a pass that measures everything, a pass with a warm cache takes less.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
DIE_TAG=final

FWD=() DRY='' STAGES='' BUDGET='' DOCS='' SIZE=$DEFAULT_PRESET
while (( $# )); do
  case $1 in
    --dry-run) DRY=1 ;;
    --stages) STAGES=${2//,/ }; shift ;;
    --budget) BUDGET=$2; shift ;;
    --no-resume) export NO_RESUME=1 ;;
    --docs) DOCS=1 ;;
    --size) SIZE=$2; shift ;;
    *) FWD+=("$1") ;;
  esac
  shift
done
preset "$SIZE" || die "unknown size $SIZE ($PRESETS)"
[[ -z $DOCS || $SIZE == standard || $SIZE == full ]] || die "--docs publishes numbers: they are only measured at --size standard or full, not $SIZE"
FWD+=(--size "$SIZE")
[[ -n $BUDGET ]] || { [[ $SIZE == full ]] && BUDGET=0 || BUDGET=25; }
SIZES=$P_ROWS; [[ $SIZE != full ]] || SIZES=1000000,10000000
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
GAPOUT=$BENCH/out/final-$STAMP
export STAGE_STATE=$STORE/final-state/$SIZE STAGE_LOG=$GAPOUT/stages.tsv
ALL_STAGES="store compression stats gap search"
[[ -n $STAGES ]] || STAGES=$ALL_STAGES
[[ $SIZE == standard || $SIZE == full || ${STAGES} != "$ALL_STAGES" ]] || STAGES=${STAGES/compression /}   # the compression variants are published at 1M: smoke and quick skip them unless named

bench() { "$BENCH/bench" "$@"; }
s_store() { bench store all "${FWD[@]}" --sizes "$SIZES" ${DOCS:+--docs}; }
s_compression() { bench store all "${FWD[@]}" --compression --sizes 1000000 --cold-runs 0 --update-sizes none ${DOCS:+--docs}; }
s_stats() { bench store stats "${FWD[@]}" --sizes "$SIZES" --engines pg,sift,sift_table,es,es_cached,ch; [[ -z $DOCS ]] || cp "$BENCH"/out/latest/store-stats-*.json "$REPO/docs/results/"; }
s_gap() {
  S_SIZES=$SIZES OUT=$GAPOUT "$STEPS/gap.sh"
  [[ -z $DOCS ]] || { for f in "$GAPOUT"/gap-*.json; do cp "$f" "$REPO/docs/results/store-${f##*/}"; done
                      python3 -I "$STEPS/gap.py" render --profile "$REPO"/docs/results/store-gap-*.json --stats "$REPO"/docs/results/store-stats-*.json; }
}
s_search() { bench all "${FWD[@]}" ${DOCS:+--docs}; }
minutes() { # STAGE: the minutes it takes when everything is measured
  case $1 in
    store) echo $(( P_MIN_SETUP + P_MIN_WARM + P_MIN_COLD + P_MIN_UPDATES )) ;;
    compression) echo "${P_MIN_COMPRESSION:-6}" ;;
    stats) echo "${P_MIN_STATS:-3}" ;;
    gap) echo "${P_MIN_GAP:-4}" ;;
    search) echo "$P_MIN_SEARCH" ;;
  esac
}
key() { python3 -I "$STEPS/benchlib.py" key "$(code_key)" "$(fingerprint)" "$1" "$SIZE" "${FWD[*]}" "$DOCS" "$SIZES"; }
resumed() { [[ -z ${NO_RESUME:-} && -s $STAGE_STATE/$1.done && $(cut -d' ' -f1 "$STAGE_STATE/$1.done") == "$(key "$1")" ]]; }

plan() { # the stages that will run, their minutes, the total against the budget
  local total=0 s m
  printf 'Release pass, size %s (bench/presets.env), stages one after another:\n' "$SIZE"
  for s in $STAGES; do
    m=$(minutes "$s")
    if resumed "$s"; then printf '  %-12s skipped: finished before with the same code, settings and host (%s)\n' "$s" "$(cut -d' ' -f2- "$STAGE_STATE/$s.done")"
    else printf '  %-12s about %2d min\n' "$s" "$m"; total=$(( total + m )); fi
  done
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
  check "ports 56280 .. 56284 are free" bash -c "! ss -ltnH | grep -qE ':(5628[0-4])\b'"
  check "no containers of a store run exist (bench/bench store clean)" bash -c '[[ -z $(docker ps -a -q --filter label=sift-store) ]]'
  printf '  %s GB free in %s, %s GB in /tmp, %s GB of memory available\n' "$(df --output=avail -BG "$STORE" | tail -1 | tr -dc 0-9)" "$STORE" "$(df --output=avail -BG /tmp | tail -1 | tr -dc 0-9)" "$(awk '/MemAvailable/ {printf "%d", $2 / 1048576}' /proc/meminfo)"
  return $fail
}

plan
preconditions || { [[ -n $DRY ]] || die 'preconditions missing (bench/bench final --dry-run)'; }
[[ -z $DRY ]] || { echo 'Dry run: nothing was started.'; exit 0; }
(( BUDGET == 0 || TOTAL <= BUDGET )) || die "the stages take about $TOTAL min, over the budget of $BUDGET min: --budget MIN, --stages LIST or a smaller --size"

mkdir -p "$GAPOUT" "$STAGE_STATE"
perf_hold
T0=$SECONDS
for s in $STAGES; do
  stage_run "$s" "$(key "$s")" "$(minutes "$s")" "s_$s"
done
bench store clean >/dev/null 2>&1 || true
log "final: done in $(( (SECONDS - T0 + 30) / 60 )) min (budget $BUDGET); stages:"
awk -F'\t' '{printf "  %-12s %5.1f min (budget %s)\n", $1, $2 / 60, $3}' "$STAGE_LOG" >&2
[[ -z $DOCS ]] || log "review git diff for README.md, docs/BENCHMARKS.md, docs/BENCH-GAP.md and docs/results/"
