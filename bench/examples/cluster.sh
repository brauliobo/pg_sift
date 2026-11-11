#!/usr/bin/env bash
# One scratch PostgreSQL cluster on tmpfs for the typified examples (bench/examples/README.md).
#
#   bench/examples/cluster.sh start     initdb (first time) + start; prints the exports for the runner
#   bench/examples/cluster.sh env       print the exports (eval "$(bench/examples/cluster.sh env)")
#   bench/examples/cluster.sh stop      stop it (fast)
#   bench/examples/cluster.sh destroy   stop and remove the directory
#
# EX_TMP (default /dev/shm/ex)  data, sockets, log; refuses a directory that is not tmpfs
# EX_ROOT                       the staged install (scripts/pgtest.sh install with SIFT_TMP=<dir> puts it in <dir>/root)
# EX_PORT (default 57191)       EX_MAINTENANCE=on  leave autovacuum and the sift background worker on (default: off,
#                               the examples VACUUM when they mean to; records say maintenance = disabled)
set -euo pipefail

TMP=${EX_TMP:-/dev/shm/ex}
PORT=${EX_PORT:-57191}
ROOT=${EX_ROOT:-/dev/shm/tex/root}
DATA=$TMP/data
SOCK=$TMP/sock
SHARE=$ROOT$(pg_config --sharedir)
LIB=$ROOT$(pg_config --pkglibdir)

env_lines() { echo "export PGHOST=$SOCK PGPORT=$PORT PGUSER=postgres PGDATABASE=postgres"; }

start() {
	[[ $(stat -f -c %T "$(dirname "$TMP")") == tmpfs ]] || { echo "cluster.sh: $TMP is not on tmpfs" >&2; exit 1; }
	mkdir -p "$SOCK"
	if [[ ! -d $DATA ]]; then
		initdb --no-sync -D "$DATA" -A trust -U postgres -E UTF8 --no-locale --no-instructions >/dev/null
		local maint=off
		[[ ${EX_MAINTENANCE:-off} != on ]] || maint=on
		cat >>"$DATA/postgresql.conf" <<EOF
port = $PORT
unix_socket_directories = '$SOCK'
listen_addresses = ''
shared_buffers = 512MB
work_mem = 64MB
maintenance_work_mem = 256MB
max_wal_size = 1GB
min_wal_size = 80MB
checkpoint_timeout = 2min
fsync = off
jit = off
autovacuum = $maint
track_io_timing = on
log_statement = none
extension_control_path = '$SHARE:\$system'
dynamic_library_path = '$LIB:\$libdir'
sift.col_auto_merge = $maint
EOF
	fi
	pg_ctl -D "$DATA" -l "$TMP/log" -w start >/dev/null
	env_lines
}

case ${1:-} in
	start) start ;;
	env) env_lines ;;
	stop) pg_ctl -D "$DATA" -m fast stop >/dev/null ;;
	destroy) pg_ctl -D "$DATA" -m fast stop >/dev/null 2>&1 || true; rm -rf "$DATA" "$SOCK" "$TMP/log"; rmdir "$TMP" 2>/dev/null || true ;;
	*) sed -n 2,11p "$0"; exit 2 ;;
esac
