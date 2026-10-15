#!/usr/bin/env bash
# Throwaway PostgreSQL 18 cluster that loads sift from the repo build output,
# without a system-wide install (extension_control_path, dynamic_library_path).
#
#   scripts/pgtest.sh install    build + install into $SIFT_TMP/root
#   scripts/pgtest.sh start      initdb (first time) + start
#   scripts/pgtest.sh psql [...] psql into the test database
#   scripts/pgtest.sh env        print exports for libpq clients (eval "$(...)")
#   scripts/pgtest.sh regress    make installcheck against the cluster
#   scripts/pgtest.sh tap [t]    TAP shell tests test/t/*.sh (or test/t/<t>.sh), own clusters
#   scripts/pgtest.sh stop       stop the cluster
#   scripts/pgtest.sh destroy    stop and remove $SIFT_TMP
#   scripts/pgtest.sh sanitizer-report  print ASan/UBSan findings (exit 1 if any)
#
# SIFT_TMP (default /tmp/sift-arch) and SIFT_PORT (56100-56169, default
# 56100) let several agents run clusters side by side; TAP clusters use
# SIFT_PORT + 1 and up. SIFT_TESTING=1 (default) builds the sift._test_*
# functions; SIFT_TESTING= builds the production library.
#
# SANITIZE=1 install builds with ASan+UBSan; install runs make clean whenever
# SANITIZE or SIFT_TESTING changes. start then preloads the sanitizer runtimes
# into the server; reports go to $SIFT_TMP/sanitizer/ (sanitizer-report).
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
TMP=${SIFT_TMP:-/tmp/sift-arch}
PORT=${SIFT_PORT:-56100}
DB=${SIFT_DB:-sift}
ROOT=$TMP/root
DATA=$TMP/data
SOCK=$TMP/sock
SANDIR=$TMP/sanitizer
SHAREDIR=$ROOT$(pg_config --sharedir)
PKGLIBDIR=$ROOT$(pg_config --pkglibdir)
export PGHOST=$SOCK PGPORT=$PORT
export SIFT_TESTING=${SIFT_TESTING-1}
FLAVOR="${SANITIZE:+asan }${SIFT_TESTING:+testing}"

(( PORT >= 56100 && PORT <= 56169 )) || { echo "SIFT_PORT must be in 56100-56169" >&2; exit 1; }

install() {
	[[ $(cat "$REPO/.build-flavor" 2>/dev/null) == "$FLAVOR" ]] || make -C "$REPO" USE_PGXS=1 clean >/dev/null
	nice make -C "$REPO" -j4 USE_PGXS=1
	echo "$FLAVOR" >"$REPO/.build-flavor"
	make -C "$REPO" USE_PGXS=1 DESTDIR="$ROOT" install >/dev/null
	echo "$FLAVOR" >"$ROOT/.flavor"
}

# The server binary is not instrumented, so the ASan runtime must be preloaded.
sanitizer_env() {
	[[ $(cat "$ROOT/.flavor" 2>/dev/null) == asan* ]] || return 0
	mkdir -p "$SANDIR"
	export LD_PRELOAD=$(gcc -print-file-name=libasan.so):$(gcc -print-file-name=libubsan.so)
	export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:log_path=$SANDIR/asan
	export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:log_path=$SANDIR/ubsan
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
	( sanitizer_env; pg_ctl -D "$DATA" -l "$TMP/postgres.log" -w start >/dev/null )
	createdb -U postgres "$DB" 2>/dev/null || true
}

stop() {
	[[ -f $DATA/postmaster.pid ]] && pg_ctl -D "$DATA" -m fast -w stop >/dev/null || true
}

tap() {
	local tests=(test/t/*.sh)
	[[ -n $1 ]] && tests=(test/t/$1.sh)
	cd "$REPO"
	# the sanitizer runtime goes into the servers (SIFT_PRELOAD) and not into prove, bash or psql
	( sanitizer_env; SIFT_PRELOAD=${LD_PRELOAD:-}; unset LD_PRELOAD; export SIFT_PRELOAD
	  SIFT_TMP=$TMP SIFT_ROOT=$ROOT SIFT_PORT=$PORT prove ${PROVE_FLAGS:-} --exec bash "${tests[@]}" )
}

case ${1:-} in
	install) install ;;
	start) start ;;
	stop) stop ;;
	psql) shift; exec psql -U postgres -d "$DB" -X "$@" ;;
	env)
		echo "export PGHOST=$SOCK PGPORT=$PORT PGUSER=postgres SIFT_ROOT=$ROOT"
		sanitizer_env
		[[ -z ${ASAN_OPTIONS:-} ]] || echo "export SIFT_SANITIZER='LD_PRELOAD=$LD_PRELOAD ASAN_OPTIONS=$ASAN_OPTIONS UBSAN_OPTIONS=$UBSAN_OPTIONS'"
		;;
	regress) make -C "$REPO" USE_PGXS=1 installcheck PGUSER=postgres ;;
	tap) tap "${2:-}" ;;
	destroy) stop; rm -rf "$TMP" ;;
	sanitizer-report) ! cat "$SANDIR"/* 2>/dev/null | grep -m 40 -A 20 -E 'ERROR: AddressSanitizer|runtime error:' ;;
	*) sed -n '2,25p' "$0"; exit 1 ;;
esac
