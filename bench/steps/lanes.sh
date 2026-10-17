# Sourced by articles.sh and places.sh: where an engine's cluster is and how many CPUs it gets.
#
# Concurrent (default): every engine has a lane of its own, a cluster with its own PGDATA and port, pinned to an equal share of
# the server CPUs (CPUS: N engines get floor(CPUS / N) each, at least 1) with its load generators on an equal share of the load
# CPUs (LOAD_CPUS), so the cells of all engines run at the same time without sharing a CPU: one cell group (a class) is measured on
# every engine in the same time window, and a drift of the host hits all of them alike. SERIAL=1: one cluster ("main") with every
# server CPU, the engines interleaved cell by cell with the order rotated every round, as before; give --cpus the share of a lane
# to measure with the same CPUs as a concurrent run (docs/BENCHMARKS.md compares the two).
#
# A built lane is cached: its stopped data directory is copied to $CACHE/lanes/<key>, the key being a hash of everything the
# database depends on (the engine's adapter, its extension binary, the corpus, the harness, PostgreSQL, the CPUs of the lane), so a
# rerun, a regress run, a report change rebuilds nothing that did not change (NO_CACHE=1 or `bench --rebuild` builds again).
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

declare -A LANE_PORT LANE_DATA LANE_CPUS LANE_LOAD LANE_CPUS0 LANE_LOAD0
CONCURRENT=${CONCURRENT-$([[ -n ${SERIAL:-} ]] || echo 1)}
LANE_CACHE=$CACHE/lanes

# lanes_init SESSION ENGINES...: SESSION (articles, places) names the clusters of the session: their ports start at LANE_PORT_BASE
lanes_init() {
  local session=$1 n=$(( $# - 1 )) i=0 e s l srv ld
  shift
  mapfile -t srv < <(cpu_list "$CPUS"); mapfile -t ld < <(cpu_list "$LOAD_CPUS")
  for e in "$@"; do
    if [[ -z $CONCURRENT ]]; then   # one cluster for all: the session's main cluster
      LANE_PORT[$e]=$(( ${TIER_PORT[$TIER]} + ${MAIN_PORT_OFFSET:-0} )) LANE_DATA[$e]=${MAIN_DATA:-$PGDATA} LANE_CPUS[$e]=$CPUS LANE_LOAD[$e]=$LOAD_CPUS
      continue
    fi
    s=$(( ${#srv[@]} / n )); (( s >= 1 )) || s=1
    l=$(( ${#ld[@]} / n )); (( l >= 1 )) || l=1
    LANE_PORT[$e]=$(( ${LANE_PORT_BASE:-$(( ${TIER_PORT[$TIER]} + 10 ))} + i )) LANE_DATA[$e]=$ROOT/pg/$TIER-$session-$e
    LANE_CPUS[$e]=$(for ((k = 0; k < s; k++)); do echo "${srv[$(( (i * s + k) % ${#srv[@]} ))]}"; done | paste -sd,)
    LANE_LOAD[$e]=$(for ((k = 0; k < l; k++)); do echo "${ld[$(( (i * l + k) % ${#ld[@]} ))]}"; done | paste -sd,)
    LANE_CPUS0[$e]=${LANE_CPUS[$e]} LANE_LOAD0[$e]=${LANE_LOAD[$e]}
    i=$(( i + 1 ))
  done
  (( ${#srv[@]} >= n )) || [[ -z $CONCURRENT ]] || log "warning: $n engines on ${#srv[@]} server CPUs: some share a CPU (--cpus)" >&2
  return 0
}
# lanes_available DATASET ENGINES...: the engines that run the dataset and are installed. The PROBE statements of the adapters need a
# server: a throwaway cluster answers them.
lanes_available() {
  local ds=$1; shift
  LANE_PORT[probe]=$(( ${TIER_PORT[$TIER]} + 9 )) LANE_DATA[probe]=$ROOT/pg/$TIER-probe LANE_CPUS[probe]=${CPUS:-} LANE_LOAD[probe]=${LOAD_CPUS:-}
  lane_destroy probe; lane_start probe
  lane_run probe available_engines "$*" "$ds"
  lane_destroy probe
  unset 'LANE_PORT[probe]' 'LANE_DATA[probe]' 'LANE_CPUS[probe]' 'LANE_LOAD[probe]'
}
# lanes_rotate ROUND ENGINES...: in round R the engine k takes the CPUs (servers and load generators) of the lane (k + R) mod n. A core
# that the host happens to load, or its hyperthread sibling, then does not stay with one engine: it is a systematic difference between
# lanes that no standard error over the rounds shows. The servers are pinned again (their backends are forked from the postmaster).
lanes_rotate() {
  local r=$1 n=$(( $# - 1 )) k=0 e from pid p
  shift
  [[ -n $CONCURRENT ]] || return 0
  local -a base=("$@")
  for e in "$@"; do
    from=${base[$(( (k + r - 1) % n ))]}
    LANE_CPUS[$e]=${LANE_CPUS0[$from]} LANE_LOAD[$e]=${LANE_LOAD0[$from]}
    if [[ -f ${LANE_DATA[$e]}/postmaster.pid ]]; then
      pid=$(head -1 "${LANE_DATA[$e]}/postmaster.pid")
      for p in "$pid" $(pgrep -P "$pid"); do taskset -a -pc "${LANE_CPUS[$e]}" "$p" >/dev/null; done
    fi
    k=$(( k + 1 ))
  done
}
# the budget as machine.sh and tables.py read it: `key=value` words
lanes_describe() {
  local e=${1:-}
  [[ -n $e ]] || e=$(printf '%s\n' "${!LANE_CPUS[@]}" | head -1)
  echo "servers=${CPUS:-all} load=${LOAD_CPUS:-all} engines=${#LANE_CPUS[@]} mode=$([[ -n $CONCURRENT ]] && echo concurrent || echo serial) lane_servers=${LANE_CPUS[$e]:-all} lane_load=${LANE_LOAD[$e]:-all}"
}
# lane_run ENGINE CMD...: CMD in the environment of the engine's cluster (port, data directory, CPUs)
lane_run() {
  local e=$1; shift
  ( export PGPORT=${LANE_PORT[$e]} PORT=${LANE_PORT[$e]} PGDATA=${LANE_DATA[$e]} CPUS=${LANE_CPUS[$e]} LOAD_CPUS=${LANE_LOAD[$e]}; "$@" )
}
lane_threads() { local n; n=$(cpu_count "${LANE_LOAD[$1]}"); echo "${THREADS:-$n}"; }   # pgbench -j of a lane
lane_start() { lane_run "$1" "$STEPS/cluster.sh" init >/dev/null; lane_run "$1" "$STEPS/cluster.sh" start >/dev/null; }
lane_destroy() { lane_run "$1" "$STEPS/cluster.sh" destroy >/dev/null 2>&1 || true; }
# the distinct clusters of the engines (the main one once in serial mode)
lane_clusters() { local e; for e in "$@"; do echo "$e ${LANE_PORT[$e]}"; done | sort -k2,2 -u | cut -d' ' -f1; }

# --- the cache of built lanes
ext_file() { # extension name -> its library
  local d
  for d in ${EXT_LIB//:/ } "$PKGLIBDIR"; do [[ -f $d/$1.so ]] && { echo "$d/$1.so"; return; }; done
}
lane_key() { # ENGINE INPUTS...: INPUTS are files (and strings) the build of the dataset depends on
  local e=$1; shift
  ( load_engine "$e"
    {
      declare -p REQUIRES SETUP INDEX HELPERS RENDER PROBE PRED RANK SOURCE EXTERNAL PLACES_SETUP PLACES_INDEX PLACES_SETUP2 PLACES_INDEX2 2>/dev/null || true
      [[ -z $REQUIRES ]] || sha256sum "$(ext_file "$REQUIRES")"
      sha256sum "$BENCH/sql/harness.sql" "$BENCH/sql/places_schema.sql"
      stat -c '%n %s %Y' "$@"
      echo "$TIER $(pg_config --version) $(cpu_count "${LANE_CPUS[$e]}") ${BUILD_WORKERS:-}"
    } | sha256sum | cut -c1-20 )
}
lane_cached() { [[ -z ${NO_CACHE:-} && -d $LANE_CACHE/$2 ]]; }   # ENGINE KEY
lane_restore() { # ENGINE KEY: the data directory of the lane from the cache (the build rows go to $RUN)
  rm -rf "${LANE_DATA[$1]:?}"; mkdir -p "$(dirname "${LANE_DATA[$1]}")"
  cp -a --reflink=auto "$LANE_CACHE/$2" "${LANE_DATA[$1]}"
}
lane_save() { # ENGINE KEY [FILE...]: stop the lane, copy its data directory into the cache with the FILEs (the build rows), start it again
  [[ -z ${NO_CACHE:-} ]] || return 0
  local e=$1 key=$2 f
  lane_run "$e" pg_ctl -D "${LANE_DATA[$e]}" -m fast stop >/dev/null
  rm -rf "$LANE_CACHE/$key.tmp"; mkdir -p "$LANE_CACHE"
  cp -a --reflink=auto "${LANE_DATA[$e]}" "$LANE_CACHE/$key.tmp"
  for f in "${@:3}"; do cp "$f" "$LANE_CACHE/$key.tmp/"; done
  rm -rf "${LANE_CACHE:?}/$key"; mv "$LANE_CACHE/$key.tmp" "$LANE_CACHE/$key"
  lane_run "$e" "$STEPS/cluster.sh" start >/dev/null
}
