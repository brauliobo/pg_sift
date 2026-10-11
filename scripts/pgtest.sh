#!/usr/bin/env bash
# Throwaway PostgreSQL 18 cluster that loads sift from the repo build output,
# without a system-wide install (extension_control_path, dynamic_library_path).
#
#   scripts/pgtest.sh install    build + install into $SIFT_TMP/root
#   scripts/pgtest.sh start      initdb (first time) + start
#   scripts/pgtest.sh psql [...] psql into the test database
#   scripts/pgtest.sh regress    make installcheck against the cluster
#   scripts/pgtest.sh stop       stop the cluster
#   scripts/pgtest.sh destroy    stop and remove $SIFT_TMP
#
# SIFT_TMP (default /tmp/sift-arch) and SIFT_PORT (56100-56169, default
# 56100) let several agents run clusters side by side.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
TMP=${SIFT_TMP:-/tmp/sift-arch}
PORT=${SIFT_PORT:-56100}
DB=${SIFT_DB:-sift}
ROOT=$TMP/root
DATA=$TMP/data
SOCK=$TMP/sock
SHAREDIR=$ROOT$(pg_config --sharedir)
PKGLIBDIR=$ROOT$(pg_config --pkglibdir)
export PGHOST=$SOCK PGPORT=$PORT

(( PORT >= 56100 && PORT <= 56169 )) || { echo "SIFT_PORT must be in 56100-56169" >&2; exit 1; }

install() {
	nice make -C "$REPO" -j4 USE_PGXS=1
	make -C "$REPO" USE_PGXS=1 DESTDIR="$ROOT" install >/dev/null
}

start() {
	mkdir -p "$SOCK"
	if [[ ! -f $DATA/PG_VERSION ]]; then
		initdb -D "$DATA" -A trust -U postgres -E UTF8 --no-locale --no-instructions >/dev/null
		cat >>"$DATA/postgresql.conf" <<-EOF
			port = $PORT
			listen_addresses = ''
			unix_socket_directories = '$SOCK'
			extension_control_path = '$SHAREDIR:\$system'
			dynamic_library_path = '$PKGLIBDIR:\$libdir'
			fsync = off
			shared_buffers = 256MB
			max_worker_processes = 16
		EOF
	fi
	pg_ctl -D "$DATA" -l "$TMP/postgres.log" -w start >/dev/null
	createdb -U postgres "$DB" 2>/dev/null || true
}

stop() {
	[[ -f $DATA/postmaster.pid ]] && pg_ctl -D "$DATA" -m fast -w stop >/dev/null || true
}

case ${1:-} in
	install) install ;;
	start) start ;;
	stop) stop ;;
	psql) shift; exec psql -U postgres -d "$DB" -X "$@" ;;
	regress) make -C "$REPO" USE_PGXS=1 installcheck PGUSER=postgres ;;
	destroy) stop; rm -rf "$TMP" ;;
	*) sed -n '2,13p' "$0"; exit 1 ;;
esac
