# Sourced by articles.sh and places.sh: where an engine's cluster is.
#
# Every engine has a lane of its own, a cluster with its own PGDATA and port (an engine outside Postgres has its server next to it).
# The lanes are built one after the other (a build is timed alone) and then all stay up, idle; a measured cell runs on one engine
# at a time, every engine free to use all the CPUs of the host, the engines interleaved in rounds (the order rotates every round).
# Nothing of another engine runs during a cell, so the engines do not compete with each other, and the pressure of the host
# (scripts/quiet.sh, logged per round and checked before each batch) says how much the rest of the machine did.
#
# A built lane can be cached: its stopped data directory (without the recycled files of the write-ahead log) goes compressed to
# $CACHE/lanes/<key>/data.tar.zst, the key being a hash of everything the database depends on (the engine's adapter, its extension
# binary, the corpus, the harness, PostgreSQL), so a rerun, a regress run, the cold-cache session of `all` rebuilds
# nothing that did not change (NO_CACHE=1 or `bench --rebuild` builds again). The cache is written when LANE_CACHE_WRITE is set (`bench
# all` of a tier whose builds take minutes, before its cold-cache session, and `--keep`) and pruned after the session (steps/footprint.sh). A lane that has one engine has no `bench`
# database after its build (adopt_db renames it): a cached lane holds what a resume needs, the engine's database.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

declare -A LANE_PORT LANE_DATA
LANE_CACHE=$CACHE/lanes

# lanes_init SESSION ENGINES...: SESSION (articles, places) names the clusters of the session: their ports start at LANE_PORT_BASE
lanes_init() {
  local session=$1 i=0 e
  shift
  for e in "$@"; do
    LANE_PORT[$e]=$(( ${LANE_PORT_BASE:-$(( ${TIER_PORT[$TIER]} + 10 ))} + i )) LANE_DATA[$e]=$ROOT/pg/$TIER-$session-$e
    i=$(( i + 1 ))
  done
}
# lanes_available DATASET ENGINES...: the engines that run the dataset and are installed. The PROBE statements of the adapters need a
# server: a throwaway cluster answers them.
lanes_available() {
  local ds=$1; shift
  LANE_PORT[probe]=$(( ${TIER_PORT[$TIER]} + 9 )) LANE_DATA[probe]=$ROOT/pg/$TIER-probe
  lane_destroy probe; lane_start probe
  lane_run probe available_engines "$*" "$ds"
  lane_destroy probe
  unset 'LANE_PORT[probe]' 'LANE_DATA[probe]'
}
# lane_run ENGINE CMD...: CMD in the environment of the engine's cluster (port, data directory)
lane_run() {
  local e=$1; shift
  ( export PGPORT=${LANE_PORT[$e]} PORT=${LANE_PORT[$e]} PGDATA=${LANE_DATA[$e]}; "$@" )
}
lane_start() { lane_run "$1" "$STEPS/cluster.sh" init >/dev/null; lane_run "$1" "$STEPS/cluster.sh" start >/dev/null; }
lane_destroy() { lane_run "$1" "$STEPS/cluster.sh" destroy >/dev/null 2>&1 || true; }

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
      echo "$TIER $(pg_config --version) ${BUILD_WORKERS:-}"
    } | sha256sum | cut -c1-20 )
}
lane_cached() { [[ -z ${NO_CACHE:-} && -s $LANE_CACHE/$2/data.tar.zst ]]; }   # ENGINE KEY
lane_restore() { # ENGINE KEY: the data directory of the lane from the cache (the build rows go to $RUN)
  rm -rf "${LANE_DATA[$1]:?}"; mkdir -p "${LANE_DATA[$1]}"
  zstd -dc "$LANE_CACHE/$2/data.tar.zst" | tar -x -C "${LANE_DATA[$1]}"
  touch "$LANE_CACHE/$2"   # the entries are pruned least recently used first (footprint.sh)
}
# lane_trim DATADIR: the write-ahead log of a stopped lane keeps the segment of the shutdown checkpoint and the next one; the others are
# files of the log that the build recycled (up to max_wal_size of them, a few hundred MB), which a start does not read and recreates
lane_trim() {
  local seg
  seg=$(pg_controldata -D "$1" | sed -n "s/^Latest checkpoint's REDO WAL file: *//p")
  [[ -n $seg ]] || { echo "lane_trim: no checkpoint in $1" >&2; return 1; }
  find "$1/pg_wal" -maxdepth 1 -type f -regex '.*/[0-9A-F]\{24\}' -printf '%f\n' | sort | awk -v s="$seg" '$0 < s || ($0 > s && ++n > 1)' | sed "s,^,$1/pg_wal/," | xargs -r rm -f
}
lane_save() { # ENGINE KEY [FILE...]: stop the lane, trim its log, cache its data directory (compressed, with the FILEs: the build rows) when
  # the cache is written (LANE_CACHE_WRITE: `all` of the ab and full tiers before their cold-cache session, --keep), start it again
  local e=$1 key=$2 f d tmp
  d=${LANE_DATA[$e]} tmp=$LANE_CACHE/$key.tmp
  lane_run "$e" pg_ctl -D "$d" -m fast stop >/dev/null
  lane_trim "$d"
  if [[ -n ${LANE_CACHE_WRITE:-} && -z ${NO_CACHE:-} ]]; then
    rm -rf "$tmp"; mkdir -p "$tmp"
    tar -c -C "$d" --exclude=server.log . | zstd -q -T"${ZSTD_JOBS:-2}" -"${LANE_CACHE_LEVEL:-3}" -o "$tmp/data.tar.zst"
    for f in "${@:3}"; do cp "$f" "$tmp/"; done
    echo "$e $TIER ${LOG_TAG:-} $(date -u +%FT%TZ)" >"$tmp/meta"
    rm -rf "${LANE_CACHE:?}/$key"; mv "$tmp" "$LANE_CACHE/$key"
  fi
  lane_run "$e" "$STEPS/cluster.sh" start >/dev/null
}
