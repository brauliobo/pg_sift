#!/usr/bin/env bash
# Usage: bench/bench duckdb [--docs] [--rows N] [--runs N] [--cpus N] [--memory SIZE] [--timeout S] [--questions LIST] [--out FILE] [--keep] [--remeasure-all | --no-cache]   (steps/duckdb.sh)
# pg_duckdb against pg_sift and PostgreSQL on the orders of the store benchmark (bench/sql/store: the same seeded CSV, the same ten questions).
# Two containers with the same CPU quota, memory limit and PostgreSQL flags (steps/lib.sh PG_FLAGS), one node each: the official PostgreSQL image with pg_sift built from this
# checkout, and the official pg_duckdb image (STORE_DUCKDB_IMAGE in versions.env). Their threads are told the same budget N (--cpus): a leader and N - 1 parallel workers for
# PostgreSQL, duckdb.threads N and N - 1 workers for the Postgres scan of pg_duckdb. steps/duckdb.py measures four lanes (PostgreSQL, pg_sift, pg_duckdb on the heap, pg_duckdb on
# the Parquet file it writes), warm and cold, the median of --runs; the timed part takes the perf lock (D_LOCK, default /tmp/sift-perf.lock).
#   --docs   write docs/results/duckdb-ROWS.tsv and run scripts/gen-docs.py (the tables and charts that quote it)
#   --rows   orders (default 200000, the `quick` size of the store benchmark)    --questions  numbers of the questions, comma separated (default all)
#   --remeasure-all   measure the rival lanes (PostgreSQL, pg_duckdb on the heap and on Parquet) again; --no-cache  neither read nor write their cache
#            The rival lanes do not depend on the pg_sift build: their rows are kept in $RIVAL_CACHE/duckdb/ by pg_duckdb and DuckDB version, rows, CPUs, runs, memory, timeout, questions and host,
#            and a later run measures only pg_sift (its answers compared with the stock plan of its own server) without starting the pg_duckdb container
#   --keep   keep the containers and the CSV    D_PORT  the first of two ports (default 56290)    IMAGE_JOBS  the make jobs of the pg_sift image (default 2)
set -euo pipefail
. "$(dirname "$0")/lib.sh"
DIE_TAG=duckdb

ROWS=200000 RUNS=3 CPUS=3 MEMORY=8g TIMEOUT=30 QUESTIONS='' OUT_FILE='' DOCS='' KEEP='' REMEASURE='' NOCACHE=''
while (( $# )); do
  case $1 in
    --docs) DOCS=1 ;;
    --rows) ROWS=$2; shift ;;
    --runs) RUNS=$2; shift ;;
    --cpus) CPUS=$2; shift ;;
    --memory) MEMORY=$2; shift ;;
    --timeout) TIMEOUT=$2; shift ;;
    --questions) QUESTIONS=$2; shift ;;
    --out) OUT_FILE=$2; shift ;;
    --keep) KEEP=1 ;;
    --remeasure-all) REMEASURE=1 ;;
    --no-cache) NOCACHE=1 ;;
    *) sed -n '2,14p' "$0"; exit 1 ;;
  esac
  shift
done
need docker psql python3 flock git
[[ $CPUS =~ ^[1-9][0-9]*$ && $ROWS =~ ^[1-9][0-9]*$ ]] || die "--cpus and --rows take whole numbers"

NAME=${D_NAME:-sift-duckdb}
PORT=${D_PORT:-56290}
DATA=${D_DATA:-$STORE/duckdb}
LOCK_FILE=${D_LOCK:-$LOCK}
SIFT_IMAGE=$NAME-pg:latest
OUT_FILE=${OUT_FILE:-$BENCH/out/duckdb-$(date -u +%Y%m%dT%H%M%S).tsv}
mkdir -p "$(dirname "$OUT_FILE")" "$DATA"

cleanup() {
  [[ -n $KEEP ]] || docker rm -fv "$NAME-sift" "$NAME-duck" >/dev/null 2>&1 || true
}
trap cleanup EXIT

RIVALS=${RIVAL_CACHE:-${BENCH_STORE:-${BENCH_WORK:-/tmp/sift-bench}}/cache/rivals}/duckdb
pull_missing "$STORE_DUCKDB_IMAGE"
digest=$(docker image inspect -f '{{index .RepoDigests 0}}' "$STORE_DUCKDB_IMAGE" | sed 's/.*@sha256:/sha256:/' | cut -c1-19)
RIVAL_DIR=$RIVALS/$(python3 -I "$STEPS/benchlib.py" key "pg_duckdb $DUCKDB_PG_VERSION $STORE_DUCKDB_IMAGE $digest" "$(fingerprint)" "rows $ROWS cpus $CPUS runs $RUNS memory $MEMORY timeout $TIMEOUT questions ${QUESTIONS:-all}")
RIVAL_ARGS=(--rivals-save "$RIVAL_DIR")
[[ -z $NOCACHE ]] || RIVAL_ARGS=()
CACHED=''
[[ -n $NOCACHE || -n $REMEASURE || ! -e $RIVAL_DIR/complete ]] || { CACHED=1 RIVAL_ARGS=(--rivals-from "$RIVAL_DIR"); }

ports_free "$PORT" 2 || exit 1
( flock -o -w 3600 9 && IMAGE_JOBS=${IMAGE_JOBS:-2} sift_image "$SIFT_IMAGE" "$STORE_PG_IMAGE" ) 9>>/tmp/sift-ci.lock   # the build lock of every job

# the same flags and the same thread budget for both servers; pg_duckdb gets its own settings for the same budget
FLAGS=("${PG_FLAGS[@]}" -c max_parallel_workers_per_gather=$((CPUS - 1)) -c max_parallel_maintenance_workers=$((CPUS - 1)))
mapfile -t CAPS < <(docker_caps "$CPUS" "$MEMORY")
PG_LABEL="sift-duckdb=$NAME"
pg_container "$NAME-sift" "$PORT" "$SIFT_IMAGE" "${FLAGS[@]}"
[[ -n $CACHED ]] || pg_container "$NAME-duck" $((PORT + 1)) "$STORE_DUCKDB_IMAGE" "${FLAGS[@]}" -c duckdb.threads="$CPUS" -c duckdb.threads_for_postgres_scan="$CPUS" -c duckdb.max_workers_per_postgres_scan=$((CPUS - 1))

pgv() { psql -X -qAt -h 127.0.0.1 -p "$1" -U postgres -d postgres -c "SHOW server_version"; }
SERVERS=(--sift-server "PostgreSQL $(pgv "$PORT") with pg_sift")
[[ -n $CACHED ]] || SERVERS+=(--duck-server "PostgreSQL $(pgv $((PORT + 1))) with pg_duckdb")
META=(--meta "orders=$ROWS"
      --meta "pg_duckdb=$DUCKDB_PG_VERSION, commit ${DUCKDB_PG_COMMIT:0:12}, image $STORE_DUCKDB_IMAGE ($digest)"
      --meta "build=$(git -C "$REPO" rev-parse --short=12 HEAD)"
      --meta "method=client-side milliseconds of one psql session per lane: warm = median of $RUNS runs after one untimed run; cold = median of $RUNS first runs in a new session after the buffer cache was emptied (the OS page cache stays); statement timeout $TIMEOUT s; $CPUS CPUs and $MEMORY per server, PostgreSQL flags of steps/lib.sh")

if [[ -n $DOCS ]]; then OUT_FILE=$REPO/docs/results/duckdb-$ROWS.tsv; fi
sift_version=$(git -C "$REPO" describe --tags --always)
log "$ROWS orders, $CPUS CPUs and $MEMORY per server, median of $RUNS${CACHED:+; the rival lanes from the cache $RIVAL_DIR}"
LOCK=$LOCK_FILE locked python3 -I "$STEPS/duckdb.py" run --sift-port "$PORT" --duck-port $((PORT + 1)) --sift-container "$NAME-sift" --duck-container "$NAME-duck" --out "$OUT_FILE" --data "$DATA" --rows "$ROWS" --runs "$RUNS" --timeout "$TIMEOUT" \
  --version "$sift_version" ${QUESTIONS:+--queries "$QUESTIONS"} "${SERVERS[@]}" "${RIVAL_ARGS[@]}" "${META[@]}"
if [[ -n $DOCS ]]; then
  python3 -I "$REPO/scripts/gen-docs.py"
  log "wrote docs/results/duckdb-$ROWS.tsv and rendered the docs"
else
  echo "$OUT_FILE"
fi
[[ -n $KEEP ]] || rm -rf "$DATA"
