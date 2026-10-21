#!/usr/bin/env bash
# Throwaway PostgreSQL cluster that loads sift from the repo build output, without a
# system-wide install (extension_control_path, dynamic_library_path). PostgreSQL 17 has no
# extension_control_path: install then copies sift into the server's own directories, as
# SIFT_SUDO (e.g. sudo) when those are not writable.
#
#   scripts/pgtest.sh install    build + install into $SIFT_TMP/root
#   scripts/pgtest.sh start      initdb (first time) + start, without autovacuum (see start)
#   scripts/pgtest.sh psql [...] psql into the test database
#   scripts/pgtest.sh env        print exports for libpq clients (eval "$(...)")
#   scripts/pgtest.sh conf       print the postgresql.conf lines that make sift loadable
#   scripts/pgtest.sh regress [t...]  make installcheck against the cluster; with test names, pg_regress runs just those,
#                                each in its own database and output directory $SIFT_TMP/regress/<t> (parallel runs)
#   scripts/pgtest.sh tap [t]    TAP shell tests test/t/*.sh (or test/t/<t>.sh), own clusters
#   scripts/pgtest.sh clean      drop the databases a job left in the cluster (all but postgres and the test database)
#   scripts/pgtest.sh stop       stop the cluster
#   scripts/pgtest.sh destroy    stop and remove $SIFT_TMP
#   scripts/pgtest.sh sanitizer-report  print ASan/UBSan findings (exit 1 if any)
#   scripts/pgtest.sh initdb-template DIR   the data directory SIFT_INITDB_TEMPLATE points at (1 MB WAL segments)
#   scripts/pgtest.sh new-data DIR [wal]    a fresh data directory (the template copied, else initdb); wal: the default 16 MB WAL
#                                           segments, for the tests whose point is WAL (always a real initdb)
#
# SIFT_TMP (default /tmp/sift-arch) and SIFT_PORT (56100-56299, default
# 56100) let several agents run clusters side by side; SIFT_ROOT (default
# $SIFT_TMP/root) lets clusters in other SIFT_TMP directories share one install; TAP clusters use
# SIFT_PORT + 1 and up. SIFT_TESTING=1 (default) builds the sift._test_*
# functions; SIFT_TESTING= builds the production library.
#
# VULKAN=1 install builds the opt-in Vulkan backend (docs/GPU.md).
# SANITIZE=1 install builds with ASan+UBSan; SIFT_HARDEN=pkg|init|1 builds with the hardening flags of scripts/harden.mk
# (docs/SAFETY.md; SIFT_POINTER_OVERFLOW=1 adds the pointer-wrap trap); install runs make clean whenever SANITIZE, SIFT_TESTING or SIFT_HARDEN changes. start then preloads the sanitizer runtimes
# into the server, with LeakSanitizer (scripts/lsan.supp); reports go to
# $SIFT_TMP/sanitizer/ (sanitizer-report). SIFT_INITDB_TEMPLATE, a data directory made by `initdb-template`, is copied
# instead of running initdb for every cluster (scripts/ci.sh makes one per run, test/t/lib.bash uses it too).
# Every cluster is under the leak gate of scripts/leakcheck.sh: start records the memory of the
# long-lived processes, stop checks the cluster, stops it cleanly (fast, so that LeakSanitizer
# reports) and scans the logs. Exit 1 on a finding.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
TMP=${SIFT_TMP:-/tmp/sift-arch}
PORT=${SIFT_PORT:-56100}
DB=${SIFT_DB:-sift}
ROOT=${SIFT_ROOT:-$TMP/root}
export SIFT_ROOT=$ROOT
DATA=$TMP/data
SOCK=$TMP/sock
SANDIR=$TMP/sanitizer
SHAREDIR=$ROOT$(pg_config --sharedir)
PKGLIBDIR=$ROOT$(pg_config --pkglibdir)
. "$(dirname "$0")/leakcheck.sh"
PG_MAJOR=$(pg_config --version | sed -n 's/^PostgreSQL \([0-9][0-9]*\).*/\1/p')
export PGHOST=$SOCK PGPORT=$PORT
export SIFT_TESTING=${SIFT_TESTING-1}
FLAVOR="${SANITIZE:+asan }${SIFT_TESTING:+testing }${SIFT_HARDEN:+harden$SIFT_HARDEN${SIFT_UBSAN_SET:+(${SIFT_UBSAN_SET// /,})}${SIFT_POINTER_OVERFLOW:++ptr} }$([[ -n ${SIFT_LTO:-} ]] && echo lto || echo nolto)${VULKAN:+ vulkan}"

(( PORT >= 56100 && PORT <= 56299 )) || { echo "SIFT_PORT must be in 56100-56299" >&2; exit 1; }

install() {
	[[ $(cat "$REPO/.build-flavor" 2>/dev/null) == "$FLAVOR" ]] || make -C "$REPO" USE_PGXS=1 clean >/dev/null
	# on the make command line: sudo resets the environment, which would install another flavor
	local flavor=(USE_PGXS=1 SIFT_TESTING="$SIFT_TESTING" SANITIZE="${SANITIZE:-}" SIFT_LTO="${SIFT_LTO:-}" SIFT_HARDEN="${SIFT_HARDEN:-}" ${SIFT_UBSAN_SET:+"SIFT_UBSAN_SET=$SIFT_UBSAN_SET"} ${SIFT_POINTER_OVERFLOW:+SIFT_POINTER_OVERFLOW=1} ${VULKAN:+VULKAN=1})
	nice make -C "$REPO" -j4 "${flavor[@]}"
	echo "$FLAVOR" >"$REPO/.build-flavor"
	if (( PG_MAJOR >= 18 )); then
		make -C "$REPO" "${flavor[@]}" DESTDIR="$ROOT" install >/dev/null
	else
		${SIFT_SUDO:-} make -C "$REPO" "${flavor[@]}" install >/dev/null
		mkdir -p "$ROOT"
	fi
	echo "$FLAVOR" >"$ROOT/.flavor"
}

# The lines that make the staged sift loadable (the server's own install on PostgreSQL 17)
conf() {
	leak_conf
	# 19 can aggregate below a join (eager aggregation) and prices that plan under some of those of the pushdown tests, which then keep the normal plan
	(( PG_MAJOR < 19 )) || echo "enable_eager_aggregate = off"
	(( PG_MAJOR >= 18 )) || return 0
	echo "extension_control_path = '$SHAREDIR:\$system'"
	echo "dynamic_library_path = '$PKGLIBDIR:\$libdir'"
}

# The server binary is not instrumented, so the ASan runtime must be preloaded.
sanitizer_env() { [[ $(cat "$ROOT/.flavor" 2>/dev/null) != asan* ]] || leak_sanitizer_env "$SANDIR"; }

# The cluster every test starts from: superuser postgres, UTF8, no locale. The clusters run with fsync off, so initdb need not sync
# either. WAL segments of 1 MB (the default 16 MB is a floor of 64 MB of pg_wal per cluster) except for KIND wal.
initdb_template() {
	local seg=(--wal-segsize=1)
	[[ ${2:-} != wal ]] || seg=()
	initdb --no-sync -D "$1" -A trust -U postgres -E UTF8 --no-locale --no-instructions "${seg[@]}" >/dev/null
}

# new_data DIR [wal]: a fresh data directory, copied from SIFT_INITDB_TEMPLATE when there is one
new_data() {
	if [[ -n ${SIFT_INITDB_TEMPLATE:-} && ${2:-} != wal ]]; then cp -a "$SIFT_INITDB_TEMPLATE" "$1"; else initdb_template "$1" "${2:-}"; fi
}

# No autovacuum in the cluster: a worker holds a snapshot while it analyzes a table, a catalog too, and VACUUM removes only the rows older
# than every snapshot of the database, so the rows a test deleted stay and its plan or its dead-document count differs now and then (the
# reloption autovacuum_enabled cannot switch off the workers' catalogs). The one job that needs it (scripts/soak.sh) turns it on for its run.
start() {
	mkdir -p "$SOCK"
	if [[ ! -f $DATA/PG_VERSION ]]; then
		new_data "$DATA"
		cat >>"$DATA/postgresql.conf" <<-EOF
			port = $PORT
			listen_addresses = ''
			unix_socket_directories = '$SOCK'
			$(conf)
			fsync = off
			max_wal_size = 48MB
			min_wal_size = 32MB
			wal_recycle = off
			shared_buffers = 64MB
			max_worker_processes = 16
			autovacuum = off
		EOF
	fi
	( sanitizer_env; pg_ctl -D "$DATA" -l "$TMP/postgres.log" -w start >/dev/null )
	createdb -U postgres "$DB" 2>/dev/null || true
	leak_baseline "$DATA"
}

stop() {
	[[ -f $DATA/postmaster.pid ]] || return 0
	local rc=0
	PGUSER=postgres leak_cluster "$DATA" || rc=1
	pg_ctl -D "$DATA" -m fast -w stop >/dev/null
	leak_after_stop "$DATA" || rc=1
	leak_scan "$TMP" || rc=1
	return $rc
}

# clean: drops every database but the template ones, postgres and the test database: what a job created
clean() {
	local d
	for d in $(psql -U postgres -d postgres -X -qAt -c "SELECT datname FROM pg_database WHERE datname NOT IN ('postgres', 'template0', 'template1', '$DB')"); do
		dropdb -U postgres --force "$d"
	done
}

# regress [TEST...]: without names, make installcheck (the Makefile's database and test list)
regress() {
	if (( ! $# )); then make -C "$REPO" USE_PGXS=1 installcheck PGUSER=postgres; return; fi
	local out=$TMP/regress/$1 rc=0
	rm -rf "$out" && mkdir -p "$out"
	PGUSER=postgres "$(dirname "$(pg_config --pgxs)")/../test/regress/pg_regress" --inputdir="$REPO/test" --outputdir="$out" \
		--bindir= --dbname="contrib_regression_$1" "$@" || rc=$?
	[[ $rc == 0 ]] || cat "$out/regression.diffs" 2>/dev/null
	return $rc
}

tap() {
	local tests=(test/t/*.sh)
	[[ -n $1 ]] && tests=(test/t/$1.sh)
	cd "$REPO"
	# the sanitizer runtime goes into the servers (SIFT_PRELOAD) and not into prove, bash or psql
	( sanitizer_env; SIFT_PRELOAD=${LD_PRELOAD:-}; unset LD_PRELOAD; export SIFT_PRELOAD
	  SIFT_TMP=$TMP SIFT_ROOT=$ROOT SIFT_PORT=$PORT prove ${PROVE_FLAGS:---failures --comments} --exec bash "${tests[@]}" )
}

case ${1:-} in
	install) install ;;
	start) start ;;
	stop) stop ;;
	conf) conf ;;
	psql) shift; exec psql -U postgres -d "$DB" -X "$@" ;;
	env)
		echo "export PGHOST=$SOCK PGPORT=$PORT PGUSER=postgres SIFT_ROOT=$ROOT"
		sanitizer_env
		[[ -z ${ASAN_OPTIONS:-} ]] || echo "export SIFT_SANITIZER='LD_PRELOAD=$LD_PRELOAD ASAN_OPTIONS=$ASAN_OPTIONS LSAN_OPTIONS=$LSAN_OPTIONS UBSAN_OPTIONS=$UBSAN_OPTIONS'"
		;;
	regress) shift; regress "$@" ;;
	initdb-template) initdb_template "$2" ;;
	new-data) new_data "$2" "${3:-}" ;;
	tap) tap "${2:-}" ;;
	clean) clean ;;
	destroy) stop || true; rm -rf "$TMP" ;;
	sanitizer-report) leak_scan "$SANDIR" ;;
	*) sed -n '2,29p' "$0"; exit 1 ;;
esac
