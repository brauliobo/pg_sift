#!/usr/bin/env bash
# Usage: bench/bench store [all|setup|run|cold|report|clean]   (bench/steps/store.sh, called by bench/bench)
# The store benchmark: ten questions about the orders of an online store on PostgreSQL, PostgreSQL with pg_sift, Elasticsearch and
# ClickHouse (bench/sql/store/, bench/steps/store.py). Every server runs alone in a docker container with the same CPU and memory limits,
# one node, on a port of 127.0.0.1; the same seeded CSV is loaded into all of them; one client times every request.
#   all     for each size: containers, data, load, warm and cold runs, containers removed; then store.json (and, with --docs, bench/results,
#           the generated blocks of README.md and docs/BENCHMARKS.md)
#   setup   one size: containers up (they stay), data generated and loaded     run, cold, updates: measure that size (updates changes the data)
#   clean   remove everything
#   report  the tables of bench/results/store.json into the generated blocks, without measuring
#   table [FILE], gap [FILE]  the warm medians / the gap tables (pg_sift against ClickHouse and Elasticsearch) of a results file; compare A B  two results files side by side
# Settings come from bench/bench as S_* variables: S_SIZES (orders, comma separated), S_CPUS, S_MEMORY, S_RUNS, S_COLD (cold runs, 0 = none),
# S_ENGINES, S_QUERIES, S_PORT, S_NAME, S_DATA, S_UPDATE_SIZES (sizes that also get the updates run, default the first; none = no updates). Images are pinned in bench/versions.env.
set -euo pipefail
. "$(dirname "$0")/lib.sh"

NAME=${S_NAME:-sift-store}
PORT=${S_PORT:-56280}                 # PostgreSQL, the pg_sift server, Elasticsearch, ClickHouse: PORT .. PORT + 3
CPUS=${S_CPUS:-4} MEMORY=${S_MEMORY:-8g}
SIZES=${S_SIZES:-1000000,10000000}
RUNS=${S_RUNS:-7} COLD=${S_COLD:-3}
UPDATE_SIZES=${S_UPDATE_SIZES:-${SIZES%%,*}}
DATA=${S_DATA:-$STORE/store}
OUT=${OUT:-$BENCH/out/store}
export STORE_NAME=$NAME STORE_PORT=$PORT STORE_CPUS=$CPUS STORE_MEMORY=$MEMORY STORE_DATA=$DATA
PG_IMAGE=$NAME-pg:latest
PG_FLAGS=(-c shared_buffers=2GB -c effective_cache_size=6GB -c work_mem=64MB -c maintenance_work_mem=512MB -c max_wal_size=8GB -c jit=off
          -c max_worker_processes=16 -c max_parallel_workers=8 -c max_parallel_workers_per_gather="$CPUS" -c max_parallel_maintenance_workers="$CPUS")
py() { python3 -I "$STEPS/store.py" "$@"; }
engine_args() { [[ -z ${S_ENGINES:-} ]] || printf -- '--engines\n%s\n' "$S_ENGINES"; [[ -z ${S_QUERIES:-} ]] || printf -- '--queries\n%s\n' "$S_QUERIES"; }
containers() { docker ps -a --filter "label=sift-store=$NAME" --format '{{.Names}}'; }
heap_gb() { echo $(( $(numfmt --from=iec "${MEMORY^^}") / 2 / 1073741824 )); }   # Elasticsearch: a heap of half the container memory

# the image of the PostgreSQL containers: the official image plus pg_sift built from the tracked files of this checkout (as scripts/replication.sh)
build_image() {
  local ctx key
  ctx=$(mktemp -d /tmp/sift-store-image.XXXXXX)
  (cd "$REPO" && git ls-files -z Makefile pg_sift.control 'pg_sift--*.sql' src | tar --null -T - -cf -) | tar -xf - -C "$ctx"
  key=$(tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner -cf - -C "$ctx" . | sha256sum | cut -c1-12)
  if [[ $(docker image inspect -f '{{index .Config.Labels "sift-store-key"}}' "$PG_IMAGE" 2>/dev/null) == "$key" ]]; then log "image $PG_IMAGE is up to date"; rm -rf "$ctx"; return; fi
  cat >"$ctx/Dockerfile" <<EOF
FROM $STORE_PG_IMAGE
LABEL sift-store-key=$key
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends build-essential liblz4-dev libzstd-dev postgresql-server-dev-\$PG_MAJOR
COPY . /src
RUN nice make -C /src -j4 USE_PGXS=1 install && rm -rf /src
EOF
  log "building $PG_IMAGE from $STORE_PG_IMAGE"
  nice docker build -q -t "$PG_IMAGE" "$ctx" >/dev/null
  rm -rf "$ctx"
}

port_free() { [[ -z $(ss -ltnH "sport = :$1") ]] || { echo "store: port $1 is in use (S_PORT / --port)" >&2; return 1; }; }
up() {
  [[ -z $(containers) ]] || { echo "store: containers of '$NAME' exist (bench/bench store clean)" >&2; return 1; }
  for i in 0 1 2 3; do port_free $((PORT + i)); done
  build_image
  docker pull -q "$STORE_ES_IMAGE" >/dev/null; docker pull -q "$STORE_CH_IMAGE" >/dev/null
  local run=(docker run -d --label "sift-store=$NAME" --cpus "$CPUS" --memory "$MEMORY" --memory-swap "$MEMORY")
  "${run[@]}" --name "$NAME-pg" -p "127.0.0.1:$PORT:5432" --shm-size 1g -e POSTGRES_HOST_AUTH_METHOD=trust "$PG_IMAGE" postgres "${PG_FLAGS[@]}" >/dev/null
  "${run[@]}" --name "$NAME-sift" -p "127.0.0.1:$((PORT + 1)):5432" --shm-size 1g -e POSTGRES_HOST_AUTH_METHOD=trust "$PG_IMAGE" postgres "${PG_FLAGS[@]}" >/dev/null
  "${run[@]}" --name "$NAME-es" -p "127.0.0.1:$((PORT + 2)):9200" -e discovery.type=single-node -e xpack.security.enabled=false -e xpack.ml.enabled=false \
    -e ES_JAVA_OPTS="-Xms$(heap_gb)g -Xmx$(heap_gb)g" "$STORE_ES_IMAGE" >/dev/null
  "${run[@]}" --name "$NAME-ch" -p "127.0.0.1:$((PORT + 3)):8123" --ulimit nofile=262144:262144 -e CLICKHOUSE_USER=bench -e CLICKHOUSE_PASSWORD=bench "$STORE_CH_IMAGE" >/dev/null
  local t=0
  until pg_isready -q -h 127.0.0.1 -p "$PORT" && pg_isready -q -h 127.0.0.1 -p $((PORT + 1)) \
        && curl -fs "http://127.0.0.1:$((PORT + 2))/_cluster/health?wait_for_status=green&timeout=5s" >/dev/null && curl -fs "http://127.0.0.1:$((PORT + 3))/ping" >/dev/null; do
    (( ++t < 300 )) || { echo "store: the servers did not start; docker logs $NAME-*" >&2; return 1; }
    sleep 1
  done
  log "containers up: $(containers | paste -sd' ') (${CPUS} CPUs, ${MEMORY} each)"
}
down() { containers | xargs -r docker rm -fv >/dev/null; }

setup() { # SIZE
  up
  py gen --rows "$1"
  py load --rows "$1" --out "$OUT"
}
measure() { # SIZE
  local extra=()
  mapfile -t extra < <(engine_args)
  locked py run --rows "$1" --out "$OUT" --runs "$RUNS" "${extra[@]}"
  (( COLD == 0 )) || locked py cold --rows "$1" --out "$OUT" --cold-runs "$COLD" "${extra[@]}"
  [[ ,$UPDATE_SIZES, == *,"$1",* ]] && updates "$1" || true
}
updates() { # SIZE: last, it changes the data
  locked py updates --rows "$1" --out "$OUT" --runs "$RUNS"
}
merge() { py merge "$OUT"/store-*.json -o "$OUT/store.json"; }
docs() {
  cp "$OUT/store.json" "$REPO/bench/results/store.json"
  report
}
report() { py render --readme "$REPO/README.md" --benchmarks "$REPO/docs/BENCHMARKS.md"; }

all() {
  local s
  mkdir -p "$OUT"
  rm -f "$OUT"/store-*.json
  trap 'down' EXIT
  for s in ${SIZES//,/ }; do
    setup "$s"
    measure "$s"
    down
  done
  trap - EXIT
  merge
  py table "$OUT/store.json"
  [[ -z ${S_DOCS:-} ]] || docs
  rm -rf "$DATA"
}
clean() {
  down
  rm -rf "$DATA"
  docker rmi "$PG_IMAGE" >/dev/null 2>&1 || true
}

case ${1:-all} in
  all) all ;;
  setup) setup "${SIZES%%,*}" ;;
  run) mkdir -p "$OUT"; S_COLD=0 measure "${SIZES%%,*}" ;;
  cold) mkdir -p "$OUT"; locked py cold --rows "${SIZES%%,*}" --out "$OUT" --cold-runs "$COLD" ;;
  updates) mkdir -p "$OUT"; updates "${SIZES%%,*}" ;;
  report) report ;;
  merge) merge ;;
  table|gap) py "$1" "${2:-$REPO/bench/results/store.json}" ;;
  compare) py compare "$2" "$3" ;;
  clean) clean ;;
  *) echo "usage: store.sh all|setup|run|cold|updates|report|merge|table|gap|compare|clean" >&2; exit 1 ;;
esac
