#!/usr/bin/env bash
# Usage: TIER=Q|F bench/steps/cluster.sh init|start|stop|status|destroy
# Settings live in $PGDATA/bench.conf, rewritten on every start. WORKERS caps the parallel workers (default 8, 3 for index builds).
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
  cat >"$PGDATA/bench.conf" <<EOF
port = $PGPORT
listen_addresses = ''
unix_socket_directories = '$SOCK'
shared_buffers = ${SHBUF:-${TIER_SHBUF[$TIER]}}
maintenance_work_mem = ${TIER_MWM[$TIER]}
work_mem = 64MB
max_worker_processes = 16
max_parallel_workers = ${WORKERS:-8}
max_parallel_workers_per_gather = 2
max_parallel_maintenance_workers = ${WORKERS:-3}
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
  limited pg_ctl -D "$PGDATA" -l "$PGDATA/server.log" -w start >/dev/null
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
