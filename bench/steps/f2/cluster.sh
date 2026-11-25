#!/usr/bin/env bash
# Sourced by bench/run_round.sh (bench/bench v6-core) and bench/scaling.sh (bench/bench scaling-io): a throwaway PostgreSQL cluster with pg_sift built from a checkout.
#   v6_cluster_init WORKTREE ROUND PORT   sets WT_PATH, SCRATCH, DATA_DIR, SOCK_DIR, LOG_FILE, PG_BIN, BUILD_SEC, GIT_SHA, GIT_BRANCH and a trap that removes the cluster
#   v6_cluster_build                      builds and installs pg_sift into /tmp/sift-bench-build/ROUND (a rebuild only when the checkout changed); never writes into the checkout
#   v6_cluster_start [GUC=VALUE...]       initdb on first use, then starts the cluster with the given settings on top of the benchmark ones; v6_cluster_restart stops and starts it again
#   v6_cluster_stop                       stops it (immediate)
#   v6_psql [psql args...]                psql on the benchmark database
# A sift.* or pg_sift.* setting is written only when the checkout defines it, so one script serves a 5.1 baseline and a 6.0 tree.
# V6_CONTROL=1: a cluster without pg_sift (the stock executor as the reference); BENCH_SCRATCH_ROOT: the directory of the cluster (default /tmp).

v6_cluster_init() {
	WT_PATH=$(cd "$1" && pwd)
	ROUND_NAME=$2
	PORT=$3
	SCRATCH=${BENCH_SCRATCH_ROOT:-/tmp}/sift-bench-$ROUND_NAME
	BUILD_CACHE=/tmp/sift-bench-build/$ROUND_NAME
	SRC_DIR=$BUILD_CACHE/src
	ROOT_DIR=$BUILD_CACHE/root
	DATA_DIR=$SCRATCH/data
	SOCK_DIR=$SCRATCH/sock
	LOG_FILE=$SCRATCH/postgres.log
	BUILD_LOG=$BUILD_CACHE/build.log
	MAKE_JOBS=${BENCH_MAKE_JOBS:-4}
	PG_BIN=$(pg_config --bindir)
	PG_MAJOR=$(pg_config --version | sed -n 's/^PostgreSQL \([0-9][0-9]*\).*/\1/p')
	SHAREDIR=$ROOT_DIR$(pg_config --sharedir)
	PKGLIBDIR=$ROOT_DIR$(pg_config --pkglibdir)
	GIT_SHA=$(git -C "$WT_PATH" rev-parse --short HEAD 2>/dev/null || echo unknown)
	GIT_BRANCH=$(git -C "$WT_PATH" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
	BUILD_SEC=0.00
	unset PGDATABASE PGSERVICE PGSERVICEFILE PGUSER PGHOST PGPORT
	trap v6_cluster_cleanup EXIT
	trap 'exit 130' INT
	trap 'exit 143' TERM
	rm -rf "$SCRATCH"
	mkdir -p "$SRC_DIR" "$ROOT_DIR" "$SOCK_DIR"
}

v6_cluster_cleanup() {
	set +e
	[[ -z ${DATA_DIR:-} || ! -f $DATA_DIR/postmaster.pid ]] || "$PG_BIN/pg_ctl" -D "$DATA_DIR" -m immediate stop >/dev/null 2>&1
	[[ -z ${SCRATCH:-} ]] || rm -rf "$SCRATCH"
}

v6_fingerprint() {
	if git -C "$WT_PATH" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
		{
			git -C "$WT_PATH" rev-parse HEAD
			git -C "$WT_PATH" status --porcelain
			git -C "$WT_PATH" diff
			git -C "$WT_PATH" diff --cached
		} 2>/dev/null | sha256sum | awk '{print $1}'
	else
		find "$WT_PATH" -maxdepth 4 -type f \( -name '*.c' -o -name '*.h' -o -name '*.sql' -o -name Makefile -o -name '*.control' \) -printf '%P %T@ %s\n' | sort | sha256sum | awk '{print $1}'
	fi
}

v6_cluster_build() {
	local fp obj dep t0
	fp=$(v6_fingerprint)
	if [[ -f $BUILD_CACHE/.fingerprint && $(<"$BUILD_CACHE/.fingerprint") == "$fp" && -f $PKGLIBDIR/pg_sift.so ]]; then
		echo "[bench] reusing the build in $BUILD_CACHE (fp=${fp:0:12})"
		return 0
	fi
	echo "[bench] syncing $WT_PATH to $SRC_DIR"
	rsync -a --delete --exclude=.git --exclude=bench/out --filter='P *.o' --filter='P *.d' --filter='P *.so' --filter='P *.bc' --filter='P *.dylib' "$WT_PATH/" "$SRC_DIR/"
	find "$SRC_DIR" -type f \( -name '*.o' -o -name '*.so' \) -size 0 -delete
	while IFS= read -r obj; do
		dep=${obj%.o}.d
		[[ -s $dep ]] || rm -f "$obj" "$dep"
	done < <(find "$SRC_DIR/src" -type f -name '*.o' 2>/dev/null)
	echo "[bench] building pg_sift (nice -n 10, make -j$MAKE_JOBS)"
	t0=$(date +%s.%N)
	if ! (cd "$SRC_DIR" && nice -n 10 make -j"$MAKE_JOBS" USE_PGXS=1 SIFT_TESTING= >"$BUILD_LOG" 2>&1 && make USE_PGXS=1 SIFT_TESTING= DESTDIR="$ROOT_DIR" install >>"$BUILD_LOG" 2>&1); then
		echo "[bench] ERROR: the build failed, end of $BUILD_LOG:" >&2
		tail -n 60 "$BUILD_LOG" >&2
		return 1
	fi
	printf '%s\n' "$fp" >"$BUILD_CACHE/.fingerprint"
	BUILD_SEC=$(awk -v a="$(date +%s.%N)" -v b="$t0" 'BEGIN { printf "%.2f", a - b }')
	echo "[bench] build and install in ${BUILD_SEC}s"
}

v6_defines() { # NAME: whether the checkout's sources define the setting sift.NAME (or pg_sift.NAME)
	grep -rqF "\"$1\"" "$WT_PATH/src"
}

v6_cluster_conf() {
	local g
	cat >>"$DATA_DIR/postgresql.conf" <<EOF
listen_addresses = ''
unix_socket_directories = '$SOCK_DIR'
port = $PORT
shared_buffers = '${BENCH_SHARED_BUFFERS:-256MB}'
max_wal_size = '2GB'
checkpoint_timeout = '30min'
fsync = off
synchronous_commit = off
full_page_writes = off
autovacuum = off
jit = off
max_worker_processes = 24
max_parallel_workers = 16
max_parallel_workers_per_gather = 4
max_parallel_maintenance_workers = 4
parallel_setup_cost = 0
parallel_tuple_cost = 0
min_parallel_table_scan_size = 0
min_parallel_index_scan_size = 0
log_min_messages = warning
EOF
	[[ -z ${V6_CONTROL:-} ]] || return 0
	cat >>"$DATA_DIR/postgresql.conf" <<EOF
shared_preload_libraries = 'pg_sift'
extension_control_path = '$SHAREDIR:\$system'
dynamic_library_path = '$PKGLIBDIR:\$libdir'
EOF
	for g in sift.col_auto_merge sift.table_auto_defrag sift.async_maintenance; do
		! v6_defines "$g" || echo "$g = off" >>"$DATA_DIR/postgresql.conf"
	done
	! v6_defines pg_sift.install_notice || echo "pg_sift.install_notice = off" >>"$DATA_DIR/postgresql.conf"
	return 0
}

v6_cluster_start() { # [GUC=VALUE...]: per start, on top of postgresql.conf (io_method, shared_buffers, ...)
	local opts=("-k" "$SOCK_DIR") g
	for g in "$@"; do opts+=("-c" "$g"); done
	if [[ ! -f $DATA_DIR/PG_VERSION ]]; then
		"$PG_BIN/initdb" --no-sync -D "$DATA_DIR" -A trust -U postgres -E UTF8 --no-locale --no-instructions >/dev/null 2>&1
		v6_cluster_conf
	fi
	if ! "$PG_BIN/pg_ctl" -D "$DATA_DIR" -l "$LOG_FILE" -w -o "${opts[*]}" start >/dev/null; then
		echo "[bench] ERROR: pg_ctl start failed:" >&2
		cat "$LOG_FILE" >&2
		return 1
	fi
	[[ $("$PG_BIN/psql" -h "$SOCK_DIR" -p "$PORT" -U postgres -d postgres -Atqc "SELECT count(*) FROM pg_database WHERE datname = 'bench'") == 1 ]] \
		|| "$PG_BIN/createdb" -h "$SOCK_DIR" -p "$PORT" -U postgres bench
}

v6_cluster_stop() {
	"$PG_BIN/pg_ctl" -D "$DATA_DIR" -m fast -w stop >/dev/null
}

v6_cluster_restart() {
	v6_cluster_stop
	v6_cluster_start "$@"
}

v6_psql() {
	"$PG_BIN/psql" -X -h "$SOCK_DIR" -p "$PORT" -U postgres -d bench -v ON_ERROR_STOP=1 "$@"
}
