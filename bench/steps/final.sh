#!/usr/bin/env bash
# Usage: bench/analytics --final [--dry-run]   (bench/steps/final.sh, called by bench/analytics)
# The one command behind the published analytics numbers, once the code is final: for PostgreSQL, pg_sift (`sift_col`, `sift_table`),
# Elasticsearch and ClickHouse it starts the containers, loads 1M and 10M orders, runs the warm, cold and update measurements, the pg_sift
# compression variants, the engines' own accounting and the pg_sift profile (private cluster under a memory cap), writes the results JSON
# into docs/results/, the tables of README.md and docs/BENCHMARKS.md and the gap report docs/BENCH-GAP.md, and removes every container.
# --dry-run prints the plan, the estimated time, disk and memory and the state of the preconditions, and starts nothing.
# Memory: every container runs with --memory (S_MEMORY, default 8g) and no swap; the pg_sift profile cluster runs under
# `systemd-run --user --scope -p MemoryMax=$GAP_MEM -p MemorySwapMax=0`. The steps run one after another.
set -euo pipefail
. "$(dirname "$0")/lib.sh"

DRY=''
[[ ${1:-} != --dry-run ]] || DRY=1
SIZES=${S_SIZES:-1000000,10000000}
GAP_MEM=${GAP_MEM:-12G}
MEMORY=${S_MEMORY:-8g}
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
GAPOUT=$BENCH/out/final-$STAMP
export GAP_MEM

# step: name | command | minutes (measured on this host in the preliminary runs, 16 threads, 4 CPUs per container)
STEPS_LIST=(
  "store: warm, cold and update tables, $SIZES orders, every system|$BENCH/bench store all --sizes $SIZES --docs|70"
  "store: compression variants of the pg_sift column index, the codecs' sizes in every system, 1000000 orders|$BENCH/bench store all --sizes 1000000 --compression --cold-runs 0 --update-sizes none --docs|8"
  "store: what ClickHouse and Elasticsearch read per question|$BENCH/bench store stats --sizes $SIZES|14"
  "pg_sift profile: planning, instructions, symbols, $SIZES orders (private cluster)|S_SIZES=$SIZES OUT=$GAPOUT $STEPS/gap.sh|8"
  "gap report: docs/BENCH-GAP.md|python3 -I $STEPS/gap.py render --profile $REPO/docs/results/store-gap-*.json --stats $REPO/docs/results/store-stats-*.json|1"
)

plan() {
  local total=0 i=1 line
  printf 'Plan (steps run one after another; every container is removed at the end of its step):\n'
  for line in "${STEPS_LIST[@]}"; do
    IFS='|' read -r name cmd min <<<"$line"
    printf '  %d. %s\n       %s\n       about %d min\n' "$i" "$name" "$cmd" "$min"
    total=$((total + min)); i=$((i + 1))
  done
  printf 'Estimated time: about %d min (a loaded host takes longer: the timed parts wait for the perf lock, %s).\n' "$total" "$LOCK"
  printf 'Estimated disk: about 30 GB in %s (the 10M CSV is 0.9 GB; the servers keep tables, indexes and up to 8 GB of WAL each) and the same in the docker root; removed afterwards.\n' "$STORE"
  printf 'Memory: 4 containers of at most %s each (no swap) = %d GiB at most, plus a cluster capped at %s for the profile; resident use measured in the preliminary runs was well below the caps.\n' \
    "$MEMORY" "$((4 * $(numfmt --from=iec "${MEMORY^^}") / 1073741824))" "$GAP_MEM"
}

check() { # DESCRIPTION COMMAND...
  local what=$1; shift
  if "$@" >/dev/null 2>&1; then printf '  ok       %s\n' "$what"; else printf '  MISSING  %s\n' "$what"; fail=1; fi
}
free_gb() { df --output=avail -BG "$1" | tail -1 | tr -dc 0-9; }

preconditions() {
  fail=0
  printf 'Preconditions:\n'
  check "docker works" docker info
  check "systemd-run --user scope works (the memory cap of the profile cluster)" systemd-run --user --scope -q -p MemoryMax=1G true
  check "psql, python3, flock, ss, perf, pgbench, pg_config" bash -c 'for c in psql python3 flock ss perf pgbench pg_config; do command -v $c || exit 1; done'
  check "pg_config is PostgreSQL 18 (the profile cluster is built for it)" bash -c 'pg_config --version | grep -q "PostgreSQL 18"'
  check "ports $((${STORE_PORT_BASE:-56280})) .. $((${STORE_PORT_BASE:-56280} + 3)) are free" bash -c "! ss -ltnH | grep -qE ':(5628[0-3])\b'"
  check "no containers of a store run exist (bench/bench store clean)" bash -c '[[ -z $(docker ps -a -q --filter label=sift-store) ]]'
  printf '  %s GB free in %s, %s GB in /tmp, %s GB of memory available (about 30 GB of disk are needed)\n' "$(free_gb "$STORE")" "$STORE" "$(free_gb /tmp)" "$(awk '/MemAvailable/ {printf "%d", $2 / 1048576}' /proc/meminfo)"
  (( $(free_gb /tmp) >= 30 )) || { echo '  MISSING  30 GB of free disk in /tmp'; fail=1; }
  for img in "$STORE_PG_IMAGE" "$STORE_ES_IMAGE" "$STORE_CH_IMAGE"; do
    if docker image inspect "$img" >/dev/null 2>&1; then printf '  ok       image %s is present\n' "$img"; else printf '  pull     image %s will be pulled\n' "$img"; fi
  done
  for n in $(tr , ' ' <<<"$SIZES"); do
    if [[ -f $STORE/store/orders-$n.csv ]]; then printf '  ok       seeded data for %s orders is present (generated again when its stamp differs)\n' "$n"; else printf '  generate the seeded data for %s orders (a minute or two)\n' "$n"; fi
  done
  return $fail
}

plan
preconditions || { [[ -n $DRY ]] || die 'preconditions missing (bench/analytics --final --dry-run)'; }
[[ -z $DRY ]] || { echo 'Dry run: nothing was started.'; exit 0; }

mkdir -p "$GAPOUT"
for line in "${STEPS_LIST[@]}"; do
  IFS='|' read -r name cmd _ <<<"$line"
  log "final: $name"
  eval "$cmd"
  case $name in
    "store: what ClickHouse"*) cp "$BENCH"/out/latest/store-stats-*.json "$REPO/docs/results/" ;;
    "pg_sift profile"*) for f in "$GAPOUT"/gap-*.json; do cp "$f" "$REPO/docs/results/store-${f##*/}"; done ;;
  esac
done
log "final: done; review git diff for README.md, docs/BENCHMARKS.md, docs/BENCH-GAP.md and docs/results/"
