#!/usr/bin/env bash
# Usage: TIER=Q|F bench/steps/cluster.sh init|start|stop|status|destroy
# Settings live in $PGDATA/bench.conf, rewritten on every start. The server is free to use every CPU of the host: its parallel workers are
# as many as the host has hardware threads (WORKERS caps them), an index build gets BUILD_WORKERS of them (default one fewer than the
# physical cores: more would only cut a small table into more segments for the engines whose parallel build makes one per worker).
# PORT and PGDATA select another cluster than the tier's (bench/steps/lanes.sh).
# Extensions come from the engine roots built by bench/steps/fetch.sh (engines/*/root) before the system ones.
. "$(dirname "$0")/lib.sh"

# Libraries the engines under test need preloaded; only those installed (an engine root or the system)
preload() {
  local libs=pg_stat_statements,pg_prewarm lib dir
  for lib in pg_textsearch pg_search; do
    for dir in ${EXT_LIB//:/ } "$PKGLIBDIR"; do
      [[ -f $dir/$lib.so ]] && { libs+=,$lib; break; }
    done
  done
  echo "$libs"
}

settings() {
  local n cores build
  n=$(nproc) cores=$(lscpu -p=core,socket | grep -v '^#' | sort -u | wc -l) build=$(( cores > 1 ? cores - 1 : 0 ))
  cat >"$PGDATA/bench.conf" <<EOF
port = $PGPORT
listen_addresses = ''
unix_socket_directories = '$SOCK'
shared_buffers = ${SHBUF:-${TIER_SHBUF[$TIER]}}
maintenance_work_mem = ${TIER_MWM[$TIER]}
work_mem = 64MB
max_worker_processes = $(( n > 8 ? 2 * n : 16 ))
max_parallel_workers = ${WORKERS:-$n}
max_parallel_workers_per_gather = 2
max_parallel_maintenance_workers = ${BUILD_WORKERS:-${WORKERS:-$build}}
# In-memory throwaway cluster: durability is irrelevant, WAL volume is still measured.
fsync = off
synchronous_commit = off
max_wal_size = 2GB
checkpoint_timeout = 30min
jit = off
huge_pages = off
shared_preload_libraries = '$(preload)'
pg_stat_statements.max = 1000
pg_stat_statements.track = top
pg_prewarm.autoprewarm = off
extension_control_path = '${EXT_SHARE:+$EXT_SHARE:}\$system'
dynamic_library_path = '${EXT_LIB:+$EXT_LIB:}\$libdir'
EOF
}

init() {
  [[ -d $PGDATA ]] && { log "$PGDATA exists"; return; }
  mkdir -p "$SOCK" "$(dirname "$PGDATA")"
  initdb -D "$PGDATA" -A trust -E UTF8 --locale=C.UTF-8 >/dev/null
  echo "include_if_exists = 'bench.conf'" >>"$PGDATA/postgresql.conf"
  log "initialized $PGDATA on port $PGPORT"
}

start() {
  pg_ctl -D "$PGDATA" status >/dev/null 2>&1 && return
  settings
  server_run pg_ctl -D "$PGDATA" -l "$PGDATA/server.log" -w start >/dev/null 8>&-   # the server must not keep the perf lock of bench
  log "started on port $PGPORT"
}

case ${1:-} in
  init)    init ;;
  start)   start ;;
  stop)    pg_ctl -D "$PGDATA" -m fast stop >/dev/null && log stopped ;;
  status)  pg_ctl -D "$PGDATA" status ;;
  destroy) pg_ctl -D "$PGDATA" -m fast stop >/dev/null 2>&1 || true; rm -rf "$PGDATA" ;;
  *) echo "usage: TIER=Q|F $0 init|start|stop|status|destroy" >&2; exit 1 ;;
esac
