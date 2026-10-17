#!/usr/bin/env bash
# A private, throwaway MariaDB server for the sift tests: its own datadir, socket and port under $SIFT_MDB_ROOT (default
# /tmp/sift-mariadb), the plugin directory pointing at a copy of the build output. No sudo, nothing outside that directory.
#
#   mariadb/server.sh start     initializes the datadir on first use, starts the server and waits for it
#   mariadb/server.sh stop      stops it (the PID it started)
#   mariadb/server.sh restart
#   mariadb/server.sh sql [args]   the mariadb client on the socket (root, no password)
#   mariadb/server.sh destroy   stops the server and deletes $SIFT_MDB_ROOT
#   mariadb/server.sh env       prints the socket path
# Variables: SIFT_MDB_ROOT, SIFT_MDB_PORT (first port to try, default 33307), SIFT_BUILD (build output, default
# /tmp/sift-mariadb-build), BUFFER_POOL (default 3G), SERVER_ARGS (extra mariadbd options).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${SIFT_MDB_ROOT:-/tmp/sift-mariadb}
BUILD=${SIFT_BUILD:-/tmp/sift-mariadb-build}
SOCK=$ROOT/mysql.sock
PIDFILE=$ROOT/mysql.pid

free_port() {
  local p=${SIFT_MDB_PORT:-33307}
  while ss -ltn "sport = :$p" | grep -q LISTEN; do p=$((p + 1)); done
  echo "$p"
}

conf() {
  cat >"$ROOT/my.cnf" <<EOF
[mysqld]
datadir=$ROOT/data
socket=$SOCK
pid-file=$PIDFILE
port=$1
bind-address=127.0.0.1
plugin_dir=$ROOT/plugin
tmpdir=$ROOT/tmp
character_set_server=utf8mb4
collation_server=utf8mb4_general_ci
max_allowed_packet=512M
skip_name_resolve
innodb_buffer_pool_size=${BUFFER_POOL:-3G}
innodb_log_file_size=512M
innodb_flush_log_at_trx_commit=2
innodb_ft_min_token_size=1
innodb_ft_enable_stopword=0
innodb_ft_result_cache_limit=1G
performance_schema=OFF
log_error=$ROOT/error.log
EOF
}

start() {
  mkdir -p "$ROOT/plugin" "$ROOT/tmp"
  cp "$BUILD/libsift_udf.so" "$ROOT/plugin/"
  if [[ ! -d $ROOT/data/mysql ]]; then
    nice mariadb-install-db --no-defaults --datadir="$ROOT/data" --basedir=/usr --auth-root-authentication-method=normal \
      --skip-test-db --skip-name-resolve >"$ROOT/install.log" 2>&1
  fi
  conf "$(free_port)"
  # shellcheck disable=SC2086  # SERVER_ARGS is a list of options
  nohup mariadbd --defaults-file="$ROOT/my.cnf" ${SERVER_ARGS:-} >"$ROOT/stdout.log" 2>&1 &
  echo $! >"$ROOT/started.pid"
  for _ in $(seq 1 120); do
    mariadb --no-defaults -uroot -S "$SOCK" -e 'select 1' >/dev/null 2>&1 && return 0
    kill -0 "$(cat "$ROOT/started.pid")" 2>/dev/null || { tail -20 "$ROOT/error.log" >&2; return 1; }
    sleep 0.5
  done
  echo "server did not start" >&2; return 1
}

stop() {
  [[ -f $ROOT/started.pid ]] || return 0
  local pid; pid=$(cat "$ROOT/started.pid")
  if kill -0 "$pid" 2>/dev/null; then
    kill "$pid"
    for _ in $(seq 1 240); do kill -0 "$pid" 2>/dev/null || break; sleep 0.5; done
    kill -0 "$pid" 2>/dev/null && kill -9 "$pid"
  fi
  rm -f "$ROOT/started.pid"
}

case ${1:-} in
  start) start ;;
  stop) stop ;;
  restart) stop; start ;;
  sql) shift; exec mariadb --no-defaults -uroot -S "$SOCK" "$@" ;;
  destroy) stop; rm -rf "$ROOT" ;;
  env) echo "$SOCK" ;;
  *) sed -n '2,12p' "$0"; exit 1 ;;
esac
