#!/usr/bin/env bash
# Usage: bench/bench duckdb [--docs] [--rows N] [--runs N] [--cpus N] [--memory SIZE] [--timeout S] [--questions LIST] [--out FILE] [--keep]   (steps/duckdb.sh)
# pg_duckdb against pg_sift and PostgreSQL on the orders of the store benchmark (bench/sql/store: the same seeded CSV, the same ten questions).
# Two containers with the same CPU quota, memory limit and PostgreSQL flags (steps/lib.sh PG_FLAGS), one node each: the official PostgreSQL image with pg_sift built from this
# checkout, and the official pg_duckdb image (STORE_DUCKDB_IMAGE in versions.env). Their threads are told the same budget N (--cpus): a leader and N - 1 parallel workers for
# PostgreSQL, duckdb.threads N and N - 1 workers for the Postgres scan of pg_duckdb. steps/duckdb.py measures four lanes (PostgreSQL, pg_sift, pg_duckdb on the heap, pg_duckdb on
# the Parquet file it writes), warm and cold, the median of --runs; the timed part takes the perf lock (D_LOCK, default /tmp/sift-perf.lock).
#   --docs   write bench/results/duckdb.tsv and the generated block of docs/BENCHMARKS.md and README.md
#   --rows   orders (default 200000, the `quick` size of the store benchmark)    --questions  numbers of the questions, comma separated (default all)
#   --keep   keep the containers and the CSV    D_PORT  the first of two ports (default 56290)    IMAGE_JOBS  the make jobs of the pg_sift image (default 2)
set -euo pipefail
. "$(dirname "$0")/lib.sh"
DIE_TAG=duckdb

ROWS=200000 RUNS=3 CPUS=3 MEMORY=8g TIMEOUT=30 QUESTIONS='' OUT_FILE='' DOCS='' KEEP=''
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
    *) sed -n '2,11p' "$0"; exit 1 ;;
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

ports_free "$PORT" 2 || exit 1
( flock -o -w 3600 9 && IMAGE_JOBS=${IMAGE_JOBS:-2} sift_image "$SIFT_IMAGE" "$STORE_PG_IMAGE" ) 9>>/tmp/sift-ci.lock   # the build lock of every job
pull_missing "$STORE_DUCKDB_IMAGE"

# the same flags and the same thread budget for both servers; pg_duckdb gets its own settings for the same budget
FLAGS=("${PG_FLAGS[@]}" -c max_parallel_workers_per_gather=$((CPUS - 1)) -c max_parallel_maintenance_workers=$((CPUS - 1)))
mapfile -t CAPS < <(docker_caps "$CPUS" "$MEMORY")
run() { # NAME PORT IMAGE [SERVER SETTINGS...]
  local n=$1 p=$2 i=$3; shift 3
  docker run -d --name "$n" --label "sift-duckdb=$NAME" "${CAPS[@]}" --shm-size 1g -p "127.0.0.1:$p:5432" -e POSTGRES_HOST_AUTH_METHOD=trust "$i" postgres "${FLAGS[@]}" "$@" >/dev/null
}
docker rm -fv "$NAME-sift" "$NAME-duck" >/dev/null 2>&1 || true
run "$NAME-sift" "$PORT" "$SIFT_IMAGE"
run "$NAME-duck" $((PORT + 1)) "$STORE_DUCKDB_IMAGE" -c duckdb.threads="$CPUS" -c duckdb.threads_for_postgres_scan="$CPUS" -c duckdb.max_workers_per_postgres_scan=$((CPUS - 1))
for p in "$PORT" $((PORT + 1)); do
  for (( t = 0; t < 300; t++ )); do pg_isready -q -h 127.0.0.1 -p "$p" && break; sleep 1; done
  pg_isready -q -h 127.0.0.1 -p "$p" || die "the server on port $p did not start (docker logs $NAME-*)"
done

pgv() { psql -X -qAt -h 127.0.0.1 -p "$1" -U postgres -d postgres -c "SHOW server_version"; }
digest=$(docker image inspect -f '{{index .RepoDigests 0}}' "$STORE_DUCKDB_IMAGE" | sed 's/.*@sha256:/sha256:/' | cut -c1-19)
META=(--meta "orders=$ROWS" --meta "postgresql=PostgreSQL $(pgv "$PORT") with pg_sift, PostgreSQL $(pgv $((PORT + 1))) with pg_duckdb"
      --meta "pg_duckdb=$DUCKDB_PG_VERSION, commit ${DUCKDB_PG_COMMIT:0:12}, image $STORE_DUCKDB_IMAGE ($digest)"
      --meta "build=$(git -C "$REPO" rev-parse --short=12 HEAD)"
      --meta "method=client-side milliseconds of one psql session per lane: warm = median of $RUNS runs after one untimed run; cold = median of $RUNS first runs in a new session after the buffer cache was emptied (the OS page cache stays); statement timeout $TIMEOUT s; $CPUS CPUs and $MEMORY per server, PostgreSQL flags of steps/lib.sh")

if [[ -n $DOCS ]]; then OUT_FILE=$REPO/bench/results/duckdb.tsv; fi
sift_version=$(git -C "$REPO" describe --tags --always)
log "$ROWS orders, $CPUS CPUs and $MEMORY per server, median of $RUNS"
flock -o -w 7200 "$LOCK_FILE" python3 -I "$STEPS/duckdb.py" run --sift-port "$PORT" --duck-port $((PORT + 1)) --out "$OUT_FILE" --data "$DATA" --rows "$ROWS" --runs "$RUNS" --timeout "$TIMEOUT" \
  --version "$sift_version" ${QUESTIONS:+--queries "$QUESTIONS"} "${META[@]}"
if [[ -n $DOCS ]]; then
  python3 -I "$STEPS/extras.py" render
  log "wrote bench/results/duckdb.tsv and the generated blocks"
else
  echo "$OUT_FILE"
fi
[[ -n $KEEP ]] || rm -rf "$DATA"
