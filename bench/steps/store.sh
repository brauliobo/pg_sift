#!/usr/bin/env bash
# Usage: bench/bench store [all|setup|run|cold|report|clean]   (bench/steps/store.sh, called by bench/bench)
# The store benchmark: ten questions about the orders of an online store on PostgreSQL, PostgreSQL with pg_sift, Citus (columnar, one node),
# Elasticsearch and ClickHouse (bench/sql/store/, bench/steps/store.py). Every server runs alone in a docker container with the same CPU and memory limits,
# one node, on a port of 127.0.0.1; the same seeded CSV is loaded into all of them; one client times every request.
# A fair comparison when the engines start different numbers of threads: the container only has the `--cpus N` quota (no pinning) and every
# engine is told the same thread budget N (PostgreSQL and Citus: a leader and N-1 parallel workers; ClickHouse max_threads and its background pool N;
# Elasticsearch node.processors and the JVM's processor count N). The timed questions run at each point of --cpus-list (1 and 3), each a result set.
#   all     for each size: containers, data, load, warm and cold runs, containers removed; then store.json (and, with --docs, bench/results,
#           the generated blocks of README.md and docs/BENCHMARKS.md)
#   setup   one size: containers up (they stay), data generated and loaded     run, cold, updates: measure that size (updates changes the data)
#           the load and the updates run at the largest budget (S_CPUS); run and cold at every point of S_CPUS_LIST, the engines reconfigured between them
#   clean   remove everything
#   report  the tables of bench/results/store.json into the generated blocks, without measuring
#   table [FILE], gap [FILE]  the warm medians / the gap tables (pg_sift against ClickHouse and Elasticsearch) of a results file; compare A B  two results files side by side
# Settings come from bench/bench as S_* variables: S_SIZES (orders, comma separated), S_CPUS (the budget of the load, default 3), S_CPUS_LIST (the budgets
# timed, comma separated, default 1,3; with S_CPUS alone just that one), S_MEMORY, S_RUNS, S_COLD (cold runs, 0 = none),
# S_ENGINES, S_QUERIES, S_PORT, S_NAME, S_DATA, S_UPDATE_SIZES (sizes that also get the updates run, default the first; none = no updates),
# S_COMPRESSION (the pg_sift `compression` variants as lanes, the size of every system with its other compression settings; results in store-compression.json). Images are pinned in bench/versions.env.
set -euo pipefail
. "$(dirname "$0")/lib.sh"

NAME=${S_NAME:-sift-store}
PORT=${S_PORT:-56280}                 # PostgreSQL, the pg_sift server, Elasticsearch, ClickHouse, Citus: PORT .. PORT + 4
CPU_LIST=${S_CPUS_LIST:-${S_CPUS:-${S_COMPRESSION:+3}}}      # the thread budgets that are timed; the compression run is one point
CPU_LIST=${CPU_LIST:-1,3}
POINTS=$(tr ',' '\n' <<<"$CPU_LIST" | sort -nu | paste -sd' ')
CPUS=${S_CPUS:-${POINTS##* }} MEMORY=${S_MEMORY:-8g}          # the budget the containers start with and the load runs at: the largest point
for c in $POINTS $CPUS; do [[ $c =~ ^[1-9][0-9]*$ ]] && (( c <= 16 )) || { echo "store: --cpus and --cpus-list take whole numbers from 1 to 16, not '$c'" >&2; exit 1; }; done
SIZES=${S_SIZES:-1000000,10000000}
RUNS=${S_RUNS:-7} COLD=${S_COLD:-3}
UPDATE_SIZES=${S_UPDATE_SIZES:-${SIZES%%,*}}
DATA=${S_DATA:-$STORE/store}
OUT=${OUT:-$BENCH/out/store}
export STORE_NAME=$NAME STORE_PORT=$PORT STORE_CPUS=$CPUS STORE_CPUS_LIST=${POINTS// /,} STORE_MEMORY=$MEMORY STORE_DATA=$DATA
PG_IMAGE=$NAME-pg:latest
CITUS_PORT=$((PORT + 4))
# The same flags for all the PostgreSQL containers (PostgreSQL, pg_sift, Citus). The parallel pool (max_worker_processes, max_parallel_workers) is far above the budget and the cost
# knobs are low, so that nothing but max_parallel_workers_per_gather (N - 1, set by `budget`) decides how many processes a scan uses.
PG_FLAGS=(-c shared_buffers=2GB -c effective_cache_size=6GB -c work_mem=64MB -c maintenance_work_mem=512MB -c max_wal_size=8GB -c jit=off
          -c max_worker_processes=16 -c max_parallel_workers=16 -c parallel_setup_cost=100 -c parallel_tuple_cost=0.01
          -c min_parallel_table_scan_size=1MB -c min_parallel_index_scan_size=64kB)
py() { python3 -I "$STEPS/store.py" "$@"; }
verify() { local extra=(); mapfile -t extra < <(engine_args); py verify --cpus "$1" "${extra[@]}"; }   # N: asks every server for its threads, dies when one differs
engine_args() { [[ -z ${S_ENGINES:-} ]] || printf -- '--engines\n%s\n' "$S_ENGINES"; [[ -z ${S_QUERIES:-} ]] || printf -- '--queries\n%s\n' "$S_QUERIES"; }
containers() { docker ps -a --filter "label=sift-store=$NAME" --format '{{.Names}}'; }
heap_gb() { echo $(( $(numfmt --from=iec "${MEMORY^^}") / 2 / 1073741824 )); }   # Elasticsearch: a heap of half the container memory

# the image of the PostgreSQL containers: the official image plus pg_sift built from the tracked files of this checkout (as scripts/replication.sh)
build_image() {
  local ctx key
  ctx=$(mktemp -d /tmp/sift-store-image.XXXXXX)
  (cd "$REPO" && git ls-files -z Makefile pg_sift.control 'pg_sift--*.sql' src scripts/harden.mk | tar --null -T - -cf -) | tar -xf - -C "$ctx"
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

want() { [[ -z ${S_ENGINES:-} || ,$S_ENGINES, == *,$1* ]]; }   # a server starts only when an engine of it is selected (S_ENGINES empty: all)
port_free() { [[ -z $(ss -ltnH "sport = :$1") ]] || { echo "store: port $1 is in use (S_PORT / --port)" >&2; return 1; }; }
volumes() { docker volume ls -q --filter "label=sift-store=$NAME"; }
cfg() { echo "$DATA/threads-$NAME"; }   # the ClickHouse thread settings of the current budget, mounted into its container
run_args() { printf '%s\n' run -d --label "sift-store=$NAME" --cpus "$1" --memory "$MEMORY" --memory-swap "$MEMORY"; }   # N: the quota, never a pinning

# Elasticsearch with N processors: the node setting (thread pools) and the JVM's count (GC, JIT); its data is in a volume, so that a new container can take over
es_up() { # N
  local run; mapfile -t run < <(run_args "$1")
  docker volume create --label "sift-store=$NAME" "$NAME-es-data" >/dev/null
  docker "${run[@]}" --name "$NAME-es" -p "127.0.0.1:$((PORT + 2)):9200" -v "$NAME-es-data:/usr/share/elasticsearch/data" \
    -e discovery.type=single-node -e xpack.security.enabled=false -e xpack.ml.enabled=false -e node.processors="$1" \
    -e ES_JAVA_OPTS="-Xms$(heap_gb)g -Xmx$(heap_gb)g -XX:ActiveProcessorCount=$1" "$STORE_ES_IMAGE" >/dev/null
}
# ClickHouse with N threads: max_threads (and the insert and FINAL threads) in the default profile, the background pool of merges and mutations in the server config
# (one task per pool thread; its defaults want 20 or more free entries to run a mutation and refuse to start with a pool this small, so the thresholds are 1).
# The logs of the server's own activity are off (query_log stays): an idle ClickHouse otherwise burns 3-7% of a CPU, which the idle check before a timed batch would count.
ch_up() { # N
  local run dir; mapfile -t run < <(run_args "$1"); dir=$(cfg)
  mkdir -p "$dir"
  cat >"$dir/server.xml" <<EOF
<clickhouse><background_pool_size>$1</background_pool_size><background_merges_mutations_concurrency_ratio>1</background_merges_mutations_concurrency_ratio>
<merge_tree><number_of_free_entries_in_pool_to_execute_mutation>1</number_of_free_entries_in_pool_to_execute_mutation><number_of_free_entries_in_pool_to_lower_max_size_of_merge>1</number_of_free_entries_in_pool_to_lower_max_size_of_merge>
<number_of_free_entries_in_pool_to_execute_optimize_entire_partition>1</number_of_free_entries_in_pool_to_execute_optimize_entire_partition></merge_tree>
<asynchronous_metrics_update_period_s>60</asynchronous_metrics_update_period_s><logger><level>warning</level></logger>
<metric_log remove="1"/><asynchronous_metric_log remove="1"/><trace_log remove="1"/><text_log remove="1"/><processors_profile_log remove="1"/><error_log remove="1"/></clickhouse>
EOF
  cat >"$dir/users.xml" <<EOF
<clickhouse><profiles><default><max_threads>$1</max_threads><max_insert_threads>$1</max_insert_threads><max_final_threads>$1</max_final_threads></default></profiles></clickhouse>
EOF
  chmod 755 "$dir"; chmod 644 "$dir"/*.xml
  docker volume create --label "sift-store=$NAME" "$NAME-ch-data" >/dev/null
  docker "${run[@]}" --name "$NAME-ch" -p "127.0.0.1:$((PORT + 3)):8123" --ulimit nofile=262144:262144 -e CLICKHOUSE_USER=bench -e CLICKHOUSE_PASSWORD=bench \
    -v "$NAME-ch-data:/var/lib/clickhouse" -v "$dir/server.xml:/etc/clickhouse-server/config.d/threads.xml:ro" -v "$dir/users.xml:/etc/clickhouse-server/users.d/threads.xml:ro" \
    "$STORE_CH_IMAGE" >/dev/null
}
# Citus: its own image (the extension is a package of it), the same PostgreSQL flags; the table is in the `columnar` access method of citus_columnar, one node
# (no workers are added), and the columnar settings (chunk group and stripe row limits, compression) stay at their defaults. The image preloads `citus`,
# which is left as shipped; the extension is only created in the `store` database as citus_columnar.
citus_up() { # N
  local run; mapfile -t run < <(run_args "$1")
  docker "${run[@]}" --name "$NAME-citus" -p "127.0.0.1:$CITUS_PORT:5432" --shm-size 1g -e POSTGRES_HOST_AUTH_METHOD=trust "$STORE_CITUS_IMAGE" postgres "${PG_FLAGS[@]}" >/dev/null
}
wait_up() {
  local t=0
  until pg_isready -q -h 127.0.0.1 -p "$PORT" && pg_isready -q -h 127.0.0.1 -p $((PORT + 1)) \
        && { ! want citus || pg_isready -q -h 127.0.0.1 -p "$CITUS_PORT"; } \
        && { ! want es || curl -fs "http://127.0.0.1:$((PORT + 2))/_cluster/health?wait_for_status=green&timeout=5s" >/dev/null; } \
        && { ! want ch || curl -fs "http://127.0.0.1:$((PORT + 3))/ping" >/dev/null; }; do
    (( ++t < 300 )) || { echo "store: the servers did not start; docker logs $NAME-*" >&2; return 1; }
    sleep 1
  done
}
# PostgreSQL, pg_sift and Citus: a leader plus N - 1 parallel workers = N processes; ALTER SYSTEM so that the same container can change budget
budget() { # N
  local p t=0
  for p in "$PORT" $((PORT + 1)) $(! want citus || echo "$CITUS_PORT"); do
    psql -X -qAt -h 127.0.0.1 -p "$p" -U postgres -d postgres -c "ALTER SYSTEM SET max_parallel_workers_per_gather = $(($1 - 1))" -c "ALTER SYSTEM SET max_parallel_maintenance_workers = $(($1 - 1))" -c "SELECT pg_reload_conf()" >/dev/null
    until [[ $(psql -X -qAt -h 127.0.0.1 -p "$p" -U postgres -d postgres -c "SHOW max_parallel_workers_per_gather") == $(($1 - 1)) ]]; do
      (( ++t < 100 )) || { echo "store: max_parallel_workers_per_gather did not change on port $p" >&2; return 1; }
      sleep 0.2
    done
  done
}
up() {
  [[ -z $(containers) ]] || { echo "store: containers of '$NAME' exist (bench/bench store clean)" >&2; return 1; }
  for i in 0 1 2 3; do port_free $((PORT + i)); done
  ! want citus || port_free "$CITUS_PORT"
  build_image
  if want es; then docker pull -q "$STORE_ES_IMAGE" >/dev/null; fi
  if want ch; then docker pull -q "$STORE_CH_IMAGE" >/dev/null; fi
  if want citus; then docker pull -q "$STORE_CITUS_IMAGE" >/dev/null; fi
  volumes | xargs -r docker volume rm >/dev/null
  local run; mapfile -t run < <(run_args "$CPUS")
  docker "${run[@]}" --name "$NAME-pg" -p "127.0.0.1:$PORT:5432" --shm-size 1g -e POSTGRES_HOST_AUTH_METHOD=trust "$PG_IMAGE" postgres "${PG_FLAGS[@]}" >/dev/null
  docker "${run[@]}" --name "$NAME-sift" -p "127.0.0.1:$((PORT + 1)):5432" --shm-size 1g -e POSTGRES_HOST_AUTH_METHOD=trust "$PG_IMAGE" postgres "${PG_FLAGS[@]}" >/dev/null
  ! want es || es_up "$CPUS"
  ! want ch || ch_up "$CPUS"
  ! want citus || citus_up "$CPUS"
  wait_up
  budget "$CPUS"
  verify "$CPUS"
  log "containers up: $(containers | paste -sd' ') (--cpus $CPUS, ${MEMORY} each)"
}
# the budget of the containers now (docker's quota), so that a development step (store.sh run) finds out where it stands
current() { echo $(( $(docker inspect -f '{{.HostConfig.NanoCpus}}' "$NAME-pg") / 1000000000 )); }
# every engine gets the thread budget N: the quota of all containers, the workers of PostgreSQL and Citus, new Elasticsearch and ClickHouse containers on their volumes (their settings are static)
configure() { # N
  log "thread budget $1: quota, PostgreSQL and Citus workers $(($1 - 1)), ClickHouse max_threads $1, Elasticsearch node.processors $1"
  containers | xargs -r docker update --cpus "$1" >/dev/null
  budget "$1"
  if want es; then docker rm -f "$NAME-es" >/dev/null; es_up "$1"; fi
  if want ch; then docker rm -f "$NAME-ch" >/dev/null; ch_up "$1"; fi
  wait_up
  verify "$1"
}
point() { (( $(current) == $1 )) || configure "$1"; }   # N: the budget the next timed step runs at
down() {
  containers | xargs -r docker rm -fv >/dev/null
  volumes | xargs -r docker volume rm >/dev/null
  rm -rf "$(cfg)"
}

setup() { # SIZE
  local extra=()
  mapfile -t extra < <(engine_args)
  up
  py gen --rows "$1"
  py load --rows "$1" --out "$OUT" --cpus "$CPUS" ${S_COMPRESSION:+--compression} "${extra[@]}"
}
run_at() { # SIZE CPUS
  local extra=()
  mapfile -t extra < <(engine_args)
  locked py run --rows "$1" --out "$OUT" --runs "$RUNS" --cpus "$2" "${extra[@]}"
}
cold_at() { # SIZE CPUS
  local extra=()
  mapfile -t extra < <(engine_args)
  locked py cold --rows "$1" --out "$OUT" --cold-runs "$COLD" --cpus "$2" "${extra[@]}"
}
each_point() { local c; for c in $POINTS; do point "$c"; "$@" "$c"; done; }   # CMD ARGS...: CMD ARGS... CPUS at every budget of the list, smallest first
at_point() { run_at "$1" "$2"; (( COLD == 0 )) || cold_at "$1" "$2"; }
measure() { # SIZE
  each_point at_point "$1"
  [[ ,$UPDATE_SIZES, == *,"$1",* ]] && updates "$1" || true
}
updates() { # SIZE: last, it changes the data; at the budget of the load
  local extra=()
  mapfile -t extra < <(engine_args)
  point "$CPUS"
  locked py updates --rows "$1" --out "$OUT" --runs "$RUNS" --cpus "$CPUS" "${extra[@]}"
}
merge() { py merge "$OUT"/store-*.json -o "$OUT/store.json"; }
docs() {
  cp "$OUT/store.json" "$REPO/bench/results/store${S_COMPRESSION:+-compression}.json"
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
}
stats() { # what ClickHouse and Elasticsearch read and spend per question, for each size (steps/store.py stats; the containers are removed after each)
  local s
  mkdir -p "$OUT"
  trap 'down' EXIT
  for s in ${SIZES//,/ }; do
    setup "$s"
    py stats --rows "$s" -o "$OUT/store-stats-$s.json"
    down
  done
  trap - EXIT
}
clean() {
  down
  rm -rf "$DATA"
  docker rmi "$PG_IMAGE" >/dev/null 2>&1 || true
}

case ${1:-all} in
  all) all ;;
  setup) setup "${SIZES%%,*}" ;;
  run) mkdir -p "$OUT"; each_point run_at "${SIZES%%,*}" ;;
  cold) mkdir -p "$OUT"; each_point cold_at "${SIZES%%,*}" ;;
  updates) mkdir -p "$OUT"; updates "${SIZES%%,*}" ;;
  report) report ;;
  stats) mkdir -p "$OUT"; stats ;;
  merge) merge ;;
  table|gap) py "$1" "${2:-$REPO/bench/results/store.json}" ;;
  compare) py compare "$2" "$3" ;;
  clean) clean ;;
  *) echo "usage: store.sh all|setup|run|cold|updates|stats|report|merge|table|gap|compare|clean" >&2; exit 1 ;;
esac
