#!/usr/bin/env bash
# Usage: bench/bench store [all|plan|setup|run|cold|updates|finish|report|clean|...]   (bench/steps/store.sh, called by bench/bench)
# The store benchmark: ten questions about the orders of an online store on PostgreSQL, PostgreSQL with pg_sift, Citus (columnar, one node),
# Elasticsearch and ClickHouse (bench/sql/store/, bench/steps/store.py). Every server runs alone in a docker container with the same CPU and memory limits,
# one node, on a port of 127.0.0.1; the same seeded CSV is loaded into all of them; one client times every request.
# A fair comparison when the engines start different numbers of threads: the container only has the `--cpus N` quota (no pinning) and every
# engine is told the same thread budget N (PostgreSQL and Citus: a leader and N-1 parallel workers; ClickHouse max_threads and its background pool N;
# Elasticsearch node.processors and the JVM's processor count N). The timed questions run at each point of --cpus-list (1 and 3), each a result set.
#   all       for each size: plan, containers, data, load, warm and cold runs, updates, containers removed; then store.json (and, with --docs, bench/results,
#             the generated blocks of README.md and docs/BENCHMARKS.md)
#   plan      one size: which servers this pass measures and which it reads from the cache of cells (plan-SIZE.json); PostgreSQL stays as the reference of the result checks
#   setup     one size: containers up (they stay), data generated and loaded (or taken from the volume that holds it)    run, cold, updates: measure that size (updates changes the data)
#             the load and the updates run at the largest budget (S_CPUS); run and cold at every point of S_CPUS_LIST, the engines reconfigured between them
#   finish    the parts of the pass, the cache and the earlier results into store.json, the cells of the servers measured into the cache, the tables
#   interference [--target E ...]   do engines that run at the same time on cores of their own change what one of them measures (store.py interference)
#   clean     remove everything (containers, data volumes, the CSV and the load records, the image); S_CLEAN_CACHE=1 also the cache of cells
#   report    the tables of bench/results/store.json into the generated blocks, without measuring
#   table [FILE], gap [FILE]  the warm medians / the gap tables (pg_sift against ClickHouse and Elasticsearch) of a results file; compare A B  two results files side by side
# Settings come from bench/bench as S_* variables, the defaults from the preset (bench/presets.env, S_PRESET): S_SIZES (orders, comma separated), S_CPUS (the budget of the load, default
# the largest point), S_CPUS_LIST (the budgets timed, comma separated; with S_CPUS alone just that one), S_MEMORY, S_RUNS, S_WARMUPS, S_COLD (cold runs, 0 = none), S_RESTARTS (runs of a
# server that is restarted between them), S_CHECK (questions also timed the other way), S_LOAD_SEC, S_PROBES, S_RATES, S_UPDATE_PATHS (engines of the update table), S_PARALLEL,
# S_ENGINES, S_QUERIES, S_PORT, S_NAME, S_DATA, S_UPDATE_SIZES (sizes that also get the updates run, default the first; none = no updates), S_CACHE_DIR, S_NO_CACHE, S_REMEASURE_ALL,
# S_REMEASURE, S_CACHE_MAX_AGE, S_MAX_RERUN, S_KEEP, S_COMPRESSION (the pg_sift `compression` variants as lanes, the size of every system with its other compression settings; results in
# store-compression.json). Images are pinned in bench/versions.env.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
DIE_TAG=store
preset "${S_PRESET:-$DEFAULT_PRESET}"

NAME=${S_NAME:-sift-store}
PORT=${S_PORT:-56280}                 # PostgreSQL, the pg_sift server, Elasticsearch, ClickHouse, Citus: PORT .. PORT + 4
CPU_LIST=${S_CPUS_LIST:-${S_CPUS:-${S_COMPRESSION:+3}}}      # the thread budgets that are timed; the compression run is one point
CPU_LIST=${CPU_LIST:-$P_CPUS_LIST}
POINTS=$(tr ',' '\n' <<<"$CPU_LIST" | sort -nu | paste -sd' ')
NPOINTS=$(wc -w <<<"$POINTS")
CPUS=${S_CPUS:-${POINTS##* }} MEMORY=${S_MEMORY:-8g}          # the budget the containers start with and the load runs at: the largest point
for c in $POINTS $CPUS; do [[ $c =~ ^[1-9][0-9]*$ ]] && (( c <= 16 )) || die "--cpus and --cpus-list take whole numbers from 1 to 16, not '$c'"; done
SIZES=${S_SIZES:-$P_ROWS}
RUNS=${S_RUNS:-$P_RUNS} WARMUPS=${S_WARMUPS:-$P_WARMUPS} COLD=${S_COLD:-$P_COLD} RESTARTS=${S_RESTARTS:-$P_RESTARTS} CHECK=${S_CHECK:-$P_CHECK}
LOAD_SEC=${S_LOAD_SEC:-$P_LOAD_SEC} PROBES=${S_PROBES:-$P_PROBES} RATES=${S_RATES:-$P_RATES} UPD_PATHS=${S_UPDATE_PATHS:-$P_UPDATE_ENGINES} PARALLEL=${S_PARALLEL:-$P_PARALLEL}
[[ $CHECK != none ]] || CHECK=''
UPDATE_SIZES=${S_UPDATE_SIZES:-${SIZES%%,*}}
DATA=${S_DATA:-$STORE/store}
CACHE_DIR=${S_CACHE_DIR:-$STORE/store-cache}   # the measured cells of the servers that are not under test (steps/benchlib.py)
OUT=${OUT:-$BENCH/out/store}
PG_IMAGE=$NAME-pg:latest
CITUS_PORT=$((PORT + 4))
export STORE_BASELINE=$OUT/baseline.json STORE_BASELINE_SEC=${S_BASELINE_SEC:-$P_BASELINE_SEC} STORE_NAME=$NAME STORE_PORT=$PORT STORE_CPUS=$CPUS STORE_CPUS_LIST=${POINTS// /,} STORE_MEMORY=$MEMORY STORE_DATA=$DATA STORE_CACHE=$CACHE_DIR STORE_PG_FLAGS="${PG_FLAGS[*]}" \
       STORE_PG_IMAGE STORE_ES_IMAGE STORE_CH_IMAGE STORE_CITUS_IMAGE
RUN_ENGINES=${S_ENGINES:-} UPD_ENGINES='' UNTIMED='' PLANNED=''   # the lanes of the load, warm and cold steps, of the updates, and the reference only (load_plan)
declare -A VOL KEY REUSE                                           # per container: its data volume, the key of what it holds, whether that load finished (data_keys)

py() { python3 -I "$STEPS/store.py" "$@"; }
verify() { local extra=(); mapfile -t extra < <(engine_args); py verify --cpus "$1" "${extra[@]}"; }   # N: asks every server for its threads, dies when one differs
flags() { # the options every measuring step of store.py takes, one per line
  printf '%s\n' --runs "$RUNS" --warmups "$WARMUPS" --cold-runs "$COLD" --restart-runs "$RESTARTS" --load-seconds "$LOAD_SEC" --probes "$PROBES" --rates "$RATES" \
    --max-rerun "${S_MAX_RERUN:-6}" --parallel-engines "$PARALLEL" --cache-dir "$CACHE_DIR" --cache-max-age "${S_CACHE_MAX_AGE:-30}"
  [[ -z $CHECK ]] || printf '%s\n' --cold-check "$CHECK"
  [[ -z ${S_NO_CACHE:-} ]] || echo --no-cache
}
engine_args() { # [LANES]: the lanes of the step (the plan's, or --engines), the reference-only lanes, the questions
  local lanes=${1:-$RUN_ENGINES}
  [[ -z $lanes ]] || printf -- '--engines\n%s\n' "$lanes"
  [[ -z $UNTIMED ]] || printf -- '--untimed\n%s\n' "$UNTIMED"
  [[ -z ${S_QUERIES:-} ]] || printf -- '--queries\n%s\n' "$S_QUERIES"
}
containers() { docker ps -a --filter "label=sift-store=$NAME" --format '{{.Names}}'; }
heap_gb() { echo $(( $(numfmt --from=iec "${MEMORY^^}") / 2 / 1073741824 )); }   # Elasticsearch: a heap of half the container memory

want() { # SERVER: it starts only when an engine of it is selected (the plan's lanes, else --engines; none: all)
  local w=${WANT:-${S_ENGINES:+,$S_ENGINES,}}
  [[ -z $w || $w == *,$1* ]]
}
cfg() { echo "$DATA/threads-$NAME"; }   # the ClickHouse thread settings of the current budget, mounted into its container
run_args() { printf '%s\n' run -d --label "sift-store=$NAME" ${BENCH_RUN_ID:+--label "sift-run=$BENCH_RUN_ID"}; docker_caps "$1" "$MEMORY"; }   # N: the quota, never a pinning

# --- what is loaded is kept: a data volume per container, named by the key of its content (the seeded rows, the image, the table definitions, the load code and settings); a pass that finds
# a volume of the key it needs whose load finished (DATA/loaded/CONTAINER-KEY.json) loads nothing. The updates change the rows, so they delete the record and `down` the volume.
wanted() { local c; for c in pg sift citus es ch; do [[ $c == pg || $c == sift ]] || want "$c" || continue; echo "$c"; done; }
data_keys() { # SIZE: key and volume of every container the pass uses (steps/store.py datakey)
  local c k v s extra=()
  mapfile -t extra < <(engine_args)
  while read -r c k v s; do KEY[$c]=$k VOL[$c]=$v REUSE[$c]=$s; done < <(py datakey --rows "$1" --cpus "$CPUS" --containers "$(wanted | paste -sd,)" ${S_COMPRESSION:+--compression} "${extra[@]}")
}
vols() { # the volumes of the containers of this pass: `up` runs in a stage (a child process) and leaves them in a file
  [[ -n ${VOLS_LOADED:-} ]] || { . "$DATA/vols-$NAME.env"; VOLS_LOADED=1; }
}
data_volume() { # CONTAINER: the volume of its key (a fresh one unless the load that filled it finished), the volumes of other keys removed
  local c=$1 v
  [[ ${REUSE[$c]} == reuse ]] || docker volume rm "${VOL[$c]}" >/dev/null 2>&1 || true
  docker volume create --label "sift-store-data=$NAME" --label "sift-store-container=$c" "${VOL[$c]}" >/dev/null
  for v in $(docker volume ls -q --filter "label=sift-store-data=$NAME" --filter "label=sift-store-container=$c"); do [[ $v == "${VOL[$c]}" ]] || docker volume rm "$v" >/dev/null 2>&1 || true; done
}
drop_stale() { # the data volumes whose load record is gone (the updates changed what they hold)
  local v c k
  for v in $(docker volume ls -q --filter "label=sift-store-data=$NAME"); do
    c=$(docker volume inspect -f '{{index .Labels "sift-store-container"}}' "$v"); k=${v##*-}
    [[ -e $DATA/loaded/$c-$k.json ]] || docker volume rm "$v" >/dev/null 2>&1 || true
  done
}
loaded() { # every container the pass uses holds the data of its key
  local c
  for c in $(wanted); do [[ -s $DATA/loaded/$c-${KEY[$c]}.json ]] || return 1; done
}

# Elasticsearch with N processors: the node setting (thread pools) and the JVM's count (GC, JIT); its data is in a volume, so that a new container can take over
es_up() { # N
  vols
  local run; mapfile -t run < <(run_args "$1")
  docker "${run[@]}" --name "$NAME-es" -p "127.0.0.1:$((PORT + 2)):9200" -v "${VOL[es]}:/usr/share/elasticsearch/data" \
    -e discovery.type=single-node -e xpack.security.enabled=false -e xpack.ml.enabled=false -e node.processors="$1" \
    -e ES_JAVA_OPTS="-Xms$(heap_gb)g -Xmx$(heap_gb)g -XX:ActiveProcessorCount=$1" "$STORE_ES_IMAGE" >/dev/null
}
# ClickHouse with N threads: max_threads (and the insert and FINAL threads) in the default profile, the background pool of merges and mutations in the server config
# (one task per pool thread; its defaults want 20 or more free entries to run a mutation and refuse to start with a pool this small, so the thresholds are 1).
# The logs of the server's own activity are off (query_log stays): an idle ClickHouse otherwise burns 3-7% of a CPU, which the idle check before a timed batch would count.
ch_up() { # N
  vols
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
  docker "${run[@]}" --name "$NAME-ch" -p "127.0.0.1:$((PORT + 3)):8123" --ulimit nofile=262144:262144 -e CLICKHOUSE_USER=bench -e CLICKHOUSE_PASSWORD=bench \
    -v "${VOL[ch]}:/var/lib/clickhouse" -v "$dir/server.xml:/etc/clickhouse-server/config.d/threads.xml:ro" -v "$dir/users.xml:/etc/clickhouse-server/users.d/threads.xml:ro" \
    "$STORE_CH_IMAGE" >/dev/null
}
# Citus: its own image (the extension is a package of it), the same PostgreSQL flags; the table is in the `columnar` access method of citus_columnar, one node
# (no workers are added), and the columnar settings (chunk group and stripe row limits, compression) stay at their defaults. The image preloads `citus`,
# which is left as shipped; the extension is only created in the `store` database as citus_columnar.
citus_up() { # N
  vols
  local run; mapfile -t run < <(run_args "$1")
  docker "${run[@]}" --name "$NAME-citus" -p "127.0.0.1:$CITUS_PORT:5432" --shm-size 1g -v "${VOL[citus]}:$(pg_mount "$STORE_CITUS_IMAGE")" -e POSTGRES_HOST_AUTH_METHOD=trust "$STORE_CITUS_IMAGE" postgres "${PG_FLAGS[@]}" >/dev/null
}
wait_up() {
  local t=0
  until pg_isready -q -h 127.0.0.1 -p "$PORT" && pg_isready -q -h 127.0.0.1 -p $((PORT + 1)) \
        && { ! want citus || pg_isready -q -h 127.0.0.1 -p "$CITUS_PORT"; } \
        && { ! want es || curl -fs "http://127.0.0.1:$((PORT + 2))/_cluster/health?wait_for_status=green&timeout=5s" >/dev/null; } \
        && { ! want ch || curl -fs "http://127.0.0.1:$((PORT + 3))/ping" >/dev/null; }; do
    (( ++t < 1500 )) || { echo "store: the servers did not start within 5 min; docker logs $NAME-*" >&2; return 1; }
    sleep 0.2
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
up() { # SIZE
  local stale; stale=$(docker ps -aq --filter "label=sift-store=$NAME" ${BENCH_RUN_ID:+--filter "label=sift-run=$BENCH_RUN_ID"})
  [[ -z $stale ]] || { log "removing the containers an earlier attempt of this run left"; xargs -r docker rm -fv <<<"$stale" >/dev/null; }
  [[ -z $(containers) ]] || { echo "store: containers of '$NAME' that belong to another run exist (bench/bench clean --run ID, or store clean)" >&2; return 1; }
  ports_free "$PORT" 4 && { ! want citus || port_free "$CITUS_PORT"; } || return 1
  sift_image "$PG_IMAGE" "$STORE_PG_IMAGE"
  pull_missing "$STORE_PG_IMAGE"
  ! want es || pull_missing "$STORE_ES_IMAGE"
  ! want ch || pull_missing "$STORE_CH_IMAGE"
  ! want citus || pull_missing "$STORE_CITUS_IMAGE"
  local c run; data_keys "$1"
  mkdir -p "$DATA"; declare -p VOL | sed 's/^declare -A/declare -gA/' >"$DATA/vols-$NAME.env"
  for c in $(wanted); do data_volume "$c"; done
  mapfile -t run < <(run_args "$CPUS")
  docker "${run[@]}" --name "$NAME-pg" -p "127.0.0.1:$PORT:5432" --shm-size 1g -v "${VOL[pg]}:$(pg_mount "$PG_IMAGE")" -e POSTGRES_HOST_AUTH_METHOD=trust "$PG_IMAGE" postgres "${PG_FLAGS[@]}" >/dev/null
  docker "${run[@]}" --name "$NAME-sift" -p "127.0.0.1:$((PORT + 1)):5432" --shm-size 1g -v "${VOL[sift]}:$(pg_mount "$PG_IMAGE")" -e POSTGRES_HOST_AUTH_METHOD=trust "$PG_IMAGE" postgres "${PG_FLAGS[@]}" >/dev/null
  ! want es || es_up "$CPUS"
  ! want ch || ch_up "$CPUS"
  ! want citus || citus_up "$CPUS"
  wait_up
  budget "$CPUS"
  verify "$CPUS"
  log "containers up: $(containers | paste -sd' ') (--cpus $CPUS, ${MEMORY} each); data: $(for c in $(wanted); do printf '%s %s ' "$c" "${REUSE[$c]}"; done)"
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
  drop_stale
  rm -rf "$(cfg)"
}

# --- the plan: which servers are measured in this pass, which are read from the cache of cells (steps/benchlib.py has the rules)
parts() { # SIZE: the parts of the pass at that size
  local p=load,warm
  if (( COLD > 0 )); then p+=,cold; fi
  if [[ ,$UPDATE_SIZES, == *,"$1",* ]]; then p+=,updates; fi
  echo "$p"
}
szdir() { echo "$OUT/$1"; }   # SIZE: where the plan and the part files of a size are; they go together, so that a changed plan drops its parts
load_plan() { # SIZE: the lanes of the plan of that size, when there is one (written by `plan`)
  local f; f=$(szdir "$1")/plan.json
  [[ -s $f ]] || return 0
  RUN_ENGINES=$(py planget "$f" run) UPD_ENGINES=$(py planget "$f" update) UNTIMED=$(py planget "$f" untimed) PLANNED=1
  WANT=",$RUN_ENGINES,$UPD_ENGINES,"
}
plan() { # SIZE: the plan of that size; a plan that differs from the one a resumed run made drops the parts measured under it
  local extra=() d; d=$(szdir "$1")
  mkdir -p "$d"
  mapfile -t extra < <(flags)
  [[ -z ${S_ENGINES:-} ]] || extra+=(--engines "$S_ENGINES")
  py plan --rows "$1" --out "$d" --plan "$d/plan.new" --cpus "$CPUS" --points "${POINTS// /,}" --parts "$(parts "$1")" --update-engines "${UPD_PATHS// /,}" \
    ${S_COMPRESSION:+--compression} ${S_REMEASURE_ALL:+--remeasure-all} ${S_REMEASURE:+--remeasure "$S_REMEASURE"} "${extra[@]}"
  if ! cmp -s "$d/plan.new" "$d/plan.json"; then rm -f "$d"/store-*.json; fi
  mv -f "$d/plan.new" "$d/plan.json"
  load_plan "$1"
}

setup() { # SIZE: containers up (they stay), the CSV generated, the data loaded unless the volumes hold it
  local extra=() c keys=() reuse=()
  up "$1"
  mapfile -t extra < <(engine_args; flags)
  py gen --rows "$1"
  for c in $(wanted); do keys+=("$c=${KEY[$c]}"); [[ ${REUSE[$c]} != reuse ]] || reuse+=("$c"); done
  py load --rows "$1" --out "$(szdir "$1")" --cpus "$CPUS" --keys "$(IFS=,; echo "${keys[*]}")" --reuse "$(IFS=,; echo "${reuse[*]:-}")" ${S_COMPRESSION:+--compression} "${extra[@]}"
}
ensure() { # SIZE: the containers are up and hold the data (the steps after the setup need that); they are started when they are not
  load_plan "$1"
  if [[ -n $(containers) ]] && { data_keys "$1"; loaded; }; then return; fi
  [[ -z $(containers) ]] || down
  setup "$1"
}

run_at() { # SIZE CPUS
  local extra=()
  mapfile -t extra < <(engine_args; flags)
  locked py run --rows "$1" --out "$(szdir "$1")" --cpus "$2" "${extra[@]}"
}
cold_at() { # SIZE CPUS
  local extra=()
  mapfile -t extra < <(engine_args; flags)
  locked py cold --rows "$1" --out "$(szdir "$1")" --cpus "$2" "${extra[@]}"
}
each_point() { local c; for c in $POINTS; do point "$c"; "$@" "$c"; done; }   # CMD ARGS...: CMD ARGS... CPUS at every budget of the list, smallest first
at_point() { run_at "$1" "$2"; }
measure() { # SIZE: warm at every budget, cold and the updates at the budget of the load (the cold runs empty the memory of one container per cell: see store.py cold)
  if [[ -n $PLANNED && -z $RUN_ENGINES ]]; then log "nothing to measure at $1 orders: every server is read from the cache"; return; fi
  stage "warm $1 orders" "$P_MIN_WARM" each_point at_point "$1"
  if (( COLD > 0 )); then point "$CPUS"; stage "cold $1 orders" "$P_MIN_COLD" cold_at "$1" "$CPUS"; fi
  if [[ ,$UPDATE_SIZES, == *,"$1",* ]]; then stage "updates $1 orders" "$P_MIN_UPDATES" updates "$1"; fi
}
updates() { # SIZE: last, it changes the data; at the budget of the load
  local extra=()
  mapfile -t extra < <(engine_args "${UPD_ENGINES:-}"; flags)
  point "$CPUS"
  locked py updates --rows "$1" --out "$(szdir "$1")" --cpus "$CPUS" "${extra[@]}"
}
section() { # SIZES: the update, delete and VACUUM section (store.py section): S_REPS repetitions (3) of the sizes up to 200,000 orders, with the churn and insert windows, one of the larger ones
  local s r extra=() secs
  mapfile -t extra < <(engine_args "${S_ENGINES:-pg,sift,sift_maint,sift_table,sift_table_maint}"; flags)
  for s in ${SIZES//,/ }; do
    for (( r = 1; r <= (s > 200000 ? 1 : ${S_REPS:-3}); r++ )); do
      secs=$(( s > 200000 ? 0 : ${S_CHURN_SECONDS:-20} ))
      mkdir -p "$(szdir "$s")"
      up "$s"
      py gen --rows "$s"
      locked py section --rows "$s" --tier "$s-r$r" --out "$(szdir "$s")" --cpus "$CPUS" --churn-seconds "$secs" "${extra[@]}" --rates ''
      down
    done
  done
  py section-merge "$OUT"/*/store-*-section.json -o "$OUT/updates.json" --sha "$(git -C "$REPO" rev-parse --short=12 HEAD)"
  [[ -z ${S_DOCS:-} ]] || { cp "$OUT/updates.json" "$REPO/bench/results/updates.json"; report; }
}
prepare() { # SIZES: what a pass needs before it times anything (the image with its build layers, the base images, the CSV of each size), so that the pass starts at once on a calm host
  local s
  build_image
  pull_missing "$STORE_PG_IMAGE"
  for s in ${SIZES//,/ }; do py gen --rows "$s"; done
}
interference() { # SIZE [store.py interference options]
  local extra=()
  mapfile -t extra < <(engine_args; flags)
  ensure "$1"; point "$CPUS"
  locked py interference --rows "$1" --out "$(szdir "$1")" --cpus "$CPUS" "${extra[@]}" "${@:2}"
}

# --- the results
plan_args() { local f; for f in "$OUT"/*/plan.json; do [[ ! -e $f ]] || printf '%s\n' --plan "$f"; done; }
merge() {
  local extra=()
  mapfile -t extra < <(plan_args)
  [[ -z ${S_DOCS:-} || -n ${S_COMPRESSION:-} || ! -s $REPO/bench/results/store.json ]] || extra+=(--keep "$REPO/bench/results/store.json")   # the sizes this pass did not measure stay in the published file
  [[ -z ${S_KEEP:-} ]] || extra+=(--keep "$S_KEEP")
  py merge "$OUT"/*/store-*.json -o "$OUT/store.json" "${extra[@]}"
}
cache_save() { # the cells of the servers this pass measured go to the cache (a compression run measures other things)
  local f
  [[ -z ${S_COMPRESSION:-} && -z ${S_NO_CACHE:-} ]] || return 0
  for f in "$OUT"/*/plan.json; do [[ ! -e $f ]] || py cache-save --plan "$f" --results "$OUT/store.json" --cache-dir "$CACHE_DIR"; done
}
docs() {
  cp "$OUT/store.json" "$REPO/bench/results/store${S_COMPRESSION:+-compression}.json"
  report
}
report() { py render --readme "$REPO/README.md" --benchmarks "$REPO/docs/BENCHMARKS.md" --img "$REPO/docs/img"; }
finish() {
  merge
  cache_save
  py table "$OUT/store.json"
  [[ -z ${S_DOCS:-} ]] || docs
}

fresh_pass() { # a pass started again into the same directory keeps what it measured when the code, host and settings are the same (resume), else starts over
  local key; key=$(python3 -I "$STEPS/benchlib.py" key "$(code_key)" "$(fingerprint)" "$(env | grep -E '^(S_|STORE_)' | sort)")
  [[ $(cat "$OUT/pass.key" 2>/dev/null) == "$key" && -z ${NO_RESUME:-} ]] || { rm -rf "${OUT:?}"/[0-9]*; log "new pass (nothing to resume in $OUT)"; }
  echo "$key" | atomic_write "$OUT/pass.key"
}
all() {
  local s
  mkdir -p "$OUT"
  fresh_pass
  trap 'down' EXIT
  log "budget (minutes, when every server is measured): setup $P_MIN_SETUP, warm $P_MIN_WARM, cold $P_MIN_COLD, updates $P_MIN_UPDATES per size, preset $P_NAME"
  for s in ${SIZES//,/ }; do
    plan "$s"
    stage "setup $s orders" "$P_MIN_SETUP" setup "$s"
    measure "$s"
    down
  done
  trap - EXIT
  finish
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
  drop_stale
  docker rmi "$PG_IMAGE" >/dev/null 2>&1 || true
  [[ -z ${S_CLEAN_CACHE:-} ]] || rm -rf "$CACHE_DIR"
}

sub=${1:-all}
[[ $sub != setup && $sub != run && $sub != cold && $sub != updates && $sub != interference && $sub != finish ]] || load_plan "${SIZES%%,*}"
case $sub in
  all) all ;;
  plan) plan "${SIZES%%,*}" ;;
  setup) mkdir -p "$OUT"; setup "${SIZES%%,*}" ;;
  run) mkdir -p "$OUT"; ensure "${SIZES%%,*}"; each_point run_at "${SIZES%%,*}" ;;
  cold) mkdir -p "$OUT"; ensure "${SIZES%%,*}"; point "$CPUS"; cold_at "${SIZES%%,*}" "$CPUS" ;;
  updates) mkdir -p "$OUT"; ensure "${SIZES%%,*}"; updates "${SIZES%%,*}" ;;
  section) mkdir -p "$OUT"; trap 'down' EXIT; section ;;
  prepare) mkdir -p "$OUT"; prepare ;;
  interference) mkdir -p "$OUT"; interference "${SIZES%%,*}" "${@:2}" ;;
  finish) finish ;;
  report) report ;;
  stats) mkdir -p "$OUT"; stats ;;
  merge) merge ;;
  table|gap) py "$1" "${2:-$REPO/bench/results/store.json}" ;;
  compare) py compare "$2" "$3" ;;
  clean) clean ;;
  *) echo "usage: store.sh all|plan|setup|run|cold|updates|section|prepare|finish|interference|stats|report|merge|table|gap|compare|clean" >&2; exit 1 ;;
esac
