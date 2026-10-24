# Shared helpers for the TAP-emitting shell tests in test/t (run them with
# `scripts/pgtest.sh tap`). Nodes live under $SIFT_TMP/tap/<name>, listen on
# unix sockets only and load sift from the staging root $SIFT_ROOT. Every
# node is stopped and its directory deleted when the script exits; logs of a
# failed run are kept. SIFT_PRELOAD (e.g. libasan.so for `make SANITIZE=1`)
# is preloaded into the servers only. finish runs the leak gate of
# scripts/leakcheck.sh on every node (cluster invariants, clean shutdown, log
# scan: leak warnings, crashes, sanitizer and LeakSanitizer reports) and fails
# the script on a finding.
set -uo pipefail
export LC_ALL=C
. "$(dirname "${BASH_SOURCE[0]}")/../../scripts/leakcheck.sh"

# knob VAR FULL QUICK: $VAR when set, else FULL, or QUICK under SIFT_QUICK=1 (scripts/ci.sh quick)
knob() { echo "${!1:-$([[ -n ${SIFT_QUICK:-} ]] && echo "$3" || echo "$2")}"; }
# the largest segment the store churn writes: 2 MB (full), 500 KB quick (8 times less live data and file growth per node; still
# several times SIFT_ALLOC_MAX_LOCKED blocks, so the segments span extents; the 20 MB writes of the tests stay)
CHURN_BYTES=$(knob SIFT_CHURN_BYTES 2000000 500000)

TAPDIR=${SIFT_TMP:?}/tap
ROOT=${SIFT_ROOT:-}
[[ -n $ROOT || ${SIFT_NODES:-} == docker ]] || { echo "run through scripts/pgtest.sh tap" >&2; exit 1; }
SHAREDIR=$ROOT$(pg_config --sharedir 2>/dev/null)
PKGLIBDIR=$ROOT$(pg_config --pkglibdir 2>/dev/null)
PG_MAJOR=$(pg_config --version 2>/dev/null | sed -n 's/^PostgreSQL \([0-9][0-9]*\).*/\1/p')
NTEST=0
NFAIL=0
NODES=()
trap cleanup EXIT

cleanup() {
	local name
	for name in "${NODES[@]}"; do
		pg_ctl -D "$(dir "$name")/data" -m immediate stop >/dev/null 2>&1
		if [[ $NFAIL -eq 0 ]]; then rm -rf "$(dir "$name")"; else rm -rf "$(dir "$name")/data"; fi
	done
}

# Megabytes used by a node's directory (data, WAL, logs)
node_mb() { du -sm "$(dir "$1")" | cut -f1; }

# TAP output
plan() { echo "1..$1"; }
ok() { NTEST=$((NTEST + 1)); echo "ok $NTEST - $*"; }
# a failed check shows the end of every node log: what the servers did around it is rarely there on a rerun
not_ok() {
	local n
	NTEST=$((NTEST + 1)); NFAIL=$((NFAIL + 1)); echo "not ok $NTEST - $*"
	for n in "${NODES[@]}"; do [[ -f $(dir "$n")/postgres.log ]] && diag "log of node $n:"$'\n'"$(tail -25 "$(dir "$n")/postgres.log" | cut -c1-240)"; done
	return 0
}
diag() { sed 's/^/# /' <<<"$*"; }
check() { local desc=$1; shift; if "$@"; then ok "$desc"; else not_ok "$desc"; fi; }
skip() { local n=$1; shift; for _ in $(seq "$n"); do NTEST=$((NTEST + 1)); echo "ok $NTEST # skip $*"; done; }
bail() { NFAIL=$((NFAIL + 1)); echo "Bail out! $*"; exit 1; }   # a bail keeps the node logs
# sed -i in place on GNU and BSD sed (BSD's takes the next word as the suffix)
sed_i() { sed -i.bak "$1" "$2" && rm -f "$2.bak"; }
# build_old REF: sets OLDROOT to the root of the released library REF (scripts/old-lib-check.sh); the test is skipped when the release cannot be built on this host
build_old() {
	local rc=0
	OLDROOT=$(SIFT_OLD_REF=$1 "$REPO/scripts/old-lib-check.sh" build) || rc=$?
	((rc != 77)) || { echo "1..0 # SKIP the release $1 cannot be built on this host (scripts/old-lib-check.sh)"; exit 0; }
	((rc == 0)) || bail "cannot build $1"
}

dir() { echo "$TAPDIR/$1"; }
port() { echo $((SIFT_PORT + $(cat "$(dir "$1")/offset"))); }

# The settings of the tests whose point is WAL, crash recovery or replication: the checkpoint cadence, the buffer pool and the
# 16 MB WAL segments decide how much WAL a crash replays and which pages are lost with it. Every other node is small (1 MB
# segments, max_wal_size and shared_buffers below). The conf of the former starts with $WAL_CONF ("$WAL_CONF
# more lines").
WAL_CONF='max_wal_size = 256MB
min_wal_size = 80MB
wal_recycle = on
shared_buffers = 128MB'

# node_init NAME OFFSET [conf lines]: fresh cluster on SIFT_PORT + OFFSET; no recycled WAL files (the file names of old
# segments are not what a test looks at, and a recycled one is disk and memory kept for nothing)
node_init() {
	local name=$1 offset=$2 d kind=
	[[ ${3:-} != "$WAL_CONF"* ]] || kind=wal
	d=$(dir "$name")
	node_stop "$name"
	rm -rf "$d"
	mkdir -p "$d/sock"
	echo "$offset" >"$d/offset"
	NODES+=("$name")
	"$(dirname "${BASH_SOURCE[0]}")/../../scripts/pgtest.sh" new-data "$d/data" $kind >"$d/initdb.log" 2>&1 || bail "initdb $name failed"
	node_conf "$name" "port = $((SIFT_PORT + offset))
listen_addresses = ''
unix_socket_directories = '$d/sock'
$("$(dirname "${BASH_SOURCE[0]}")/../../scripts/pgtest.sh" conf)
fsync = off
max_wal_size = 48MB
min_wal_size = 32MB
wal_recycle = off
restart_after_crash = off
shared_buffers = 32MB
max_connections = 60
${3:-}"
}

# node_clone PRIMARY NAME OFFSET [conf lines]: hot standby streaming from a base backup of PRIMARY
# (with a slot, so a churn outrunning a slow standby on a loaded CI runner cannot recycle the WAL it still needs)
node_clone() {
	local d
	d=$(dir "$2")
	node_stop "$2"
	rm -rf "$d"
	mkdir -p "$d/sock"
	echo "$3" >"$d/offset"
	NODES+=("$2")
	pg_basebackup -h "$(dir "$1")/sock" -p "$(port "$1")" -U postgres -D "$d/data" -R -C -S "$2" -X stream -c fast \
		>"$d/basebackup.log" 2>&1 || bail "pg_basebackup $2 failed"
	node_conf "$2" "port = $(port "$2")
unix_socket_directories = '$d/sock'
hot_standby = on
${4:-}"
}

node_promote() { pg_ctl -D "$(dir "$1")/data" -w promote >/dev/null; }

# libpq connection string of a node, for CREATE SUBSCRIPTION
conninfo() { echo "host=$(dir "$1")/sock port=$(port "$1") user=postgres dbname=postgres"; }

node_conf() { printf '%s\n' "$2" >>"$(dir "$1")/data/postgresql.conf"; }
# use_root NODE ROOT: NODE loads the sift installed under ROOT (a released library built by scripts/old-lib-check.sh, or $newroot, the one of this
# tree). Only this tree defines the settings scripts/pgtest.sh conf sets (sift.table_auto_defrag, sift.col_auto_merge, sift.col_auto_merge_wal_pct): a released
# library warns that "sift" is a reserved prefix in the first statement of a session, and the test compares what the statements print.
use_root() {
	local conf=$(dir "$1")/data/postgresql.conf k
	ROOT=$2
	for k in table_auto_defrag col_auto_merge col_auto_merge_wal_pct; do sed_i "/^sift\.$k = /d" "$conf"; done
	[[ $ROOT != "$newroot" ]] || "$(dirname "${BASH_SOURCE[0]}")/../../scripts/pgtest.sh" conf | grep -E '^sift\.(table_auto_defrag|col_auto_merge|col_auto_merge_wal_pct) = ' >>"$conf"
	node_conf "$1" "extension_control_path = '$ROOT$(pg_config --sharedir):\$system'
dynamic_library_path = '$ROOT$(pg_config --pkglibdir):\$libdir'"
}
node_start() {
	LD_PRELOAD=${SIFT_PRELOAD:-} pg_ctl -D "$(dir "$1")/data" -l "$(dir "$1")/postgres.log" -w -t 120 start \
		>/dev/null 2>&1 || return
	leak_baseline "$(dir "$1")/data"
}
node_stop() { [[ -f $(dir "$1")/data/postmaster.pid ]] && pg_ctl -D "$(dir "$1")/data" -m fast -w stop >/dev/null 2>&1; true; }

# sql NAME [QUERY...]: unaligned tuples, stops at the first error (stderr
# kept); without queries, runs the script on stdin
sql() {
	local name=$1
	shift
	PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir "$name")/sock" \
		-p "$(port "$name")" -U postgres -d postgres "${@/#/--command=}"
}

# The groups of the elements of orders.doc->'items' (the table of the sift_col tests) per category: from the column source and its
# pre-aggregates ($2 = on) or from the normal plan ($2 = off), as one string; $3 = explain: the units the pre-aggregates answered
ELEM_QUERY="SELECT e.category, count(*), sum(e.qty) FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.doc->'items') e(category text, qty int) GROUP BY 1"
elem_groups() {
	local run="SELECT coalesce(string_agg(x::text, ',' ORDER BY x::text), '') FROM ($ELEM_QUERY) x"
	[[ ${3:-} == explain ]] && run="EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $ELEM_QUERY"
	sql "$1" "LOAD 'pg_sift'" "SET sift.enable_agg = $2" "SET sift.agg_preagg = $2" "SET max_parallel_workers_per_gather = 0" \
		"SET seq_page_cost = 10000" "$run" | tail -n +1 | grep -v '^$' | { [[ ${3:-} == explain ]] && grep -o 'Units From Pre-aggregates: [0-9]*' || cat; }
}

# kill -9 the postmaster and every child at once, as a power loss would; the postmaster is stopped
# first so that it cannot fork a parallel worker that would outlive the list of children. Stop every child before
# killing the postmaster: a running parallel leader otherwise observes its death and enters fatal-error cleanup.
crash() {
	local pm kids pid state
	pm=$(head -1 "$(dir "$1")/data/postmaster.pid")
	kill -STOP "$pm" 2>/dev/null
	while [[ $(cut -d' ' -f3 "/proc/$pm/stat" 2>/dev/null) =~ ^[RSD]$ ]]; do :; done
	kids=$(pgrep -P "$pm" | tr '\n' ' ')
	kill -STOP $kids 2>/dev/null
	for pid in $kids; do
		while state=$(ps -o stat= -p "$pid"); do
			[[ $state =~ ^[[:space:]]*[TZ] ]] && break
			sleep 0.001
		done
	done
	kill -9 $kids "$pm" 2>/dev/null
	while kill -0 "$pm" $kids 2>/dev/null; do sleep 0.02; done
}

# Waits until standby $2 replayed everything primary $1 wrote so far
catchup() {
	local lsn
	lsn=$(sql "$1" "SELECT pg_current_wal_insert_lsn()")
	for _ in $(seq 600); do
		[[ $(sql "$2" "SELECT pg_last_wal_replay_lsn() >= '$lsn'") == t ]] && return 0
		sleep 0.1
	done
	return 1
}

# churn NAME IDX MAXBYTES [name:weight:sql ...]: pgbench -f arguments for a
# churn mix (weight 100) plus CHECKPOINT (weight 1) and extra scripts
churn() {
	local d f spec
	d=$(dir "$1")
	printf '\\set seed random(1, 9223372036854775806)\nSELECT sift._test_churn(%s, :seed, %s);\n' \
		"'$2'" "$3" >"$d/churn.sql"
	echo "CHECKPOINT;" >"$d/ckpt.sql"
	echo "-f $d/churn.sql@100 -f $d/ckpt.sql@1"
	shift 3
	for spec in "$@"; do
		f=$d/${spec%%:*}.sql
		printf '%s\n' "${spec#*:*:}" >"$f"
		spec=${spec#*:}
		echo "-f $f@${spec%%:*}"
	done
}

# pgbench NAME CLIENTS SECONDS ARGS...: runs in the background, pid in PGBENCH_PID
pgbench_bg() {
	local name=$1 clients=$2 secs=$3
	shift 3
	pgbench -n -h "$(dir "$name")/sock" -p "$(port "$name")" -U postgres -c "$clients" -j 4 -T "$secs" "$@" \
		postgres >"$(dir "$name")/pgbench.log" 2>&1 &
	PGBENCH_PID=$!
}

# crash_cycle NAME LABEL MAXSLEEP CLIENTS ARGS...: pgbench (600 s at most) under CLIENTS clients, killed with the cluster after a random delay
# of up to MAXSLEEP seconds, then the restart; false (not ok LABEL, the log) when the node does not come back
crash_cycle() {
	local name=$1 label=$2 max=$3 clients=$4
	shift 4
	pgbench_bg "$name" "$clients" 600 "$@"
	sleep "$((RANDOM % max)).$((RANDOM % 9 + 1))$((RANDOM % 10))"
	crash "$name"
	wait "$PGBENCH_PID"
	node_start "$name" && return 0
	not_ok "$label: restart"
	diag "$(tail -20 "$(dir "$name")/postgres.log")"
	return 1
}

# node_leakcheck NAME: the cluster invariants while it runs, a clean shutdown (LeakSanitizer reports as the processes exit)
# and the scan of its log
node_leakcheck() {
	local d rc=0
	d=$(dir "$1")
	if [[ -f $d/data/postmaster.pid ]]; then
		PGHOST=$d/sock PGPORT=$(port "$1") PGUSER=postgres leak_cluster "$d/data" || rc=1
		pg_ctl -D "$d/data" -m fast -w -t 120 stop >/dev/null 2>&1 || pg_ctl -D "$d/data" -m immediate stop >/dev/null 2>&1
		leak_after_stop "$d/data" || rc=1
	fi
	leak_scan "$d" || rc=1
	return $rc
}

finish() {
	local name out
	for name in "${NODES[@]}"; do
		out=$(node_leakcheck "$name" 2>&1) || { not_ok "leak check of node $name"; diag "$out"; }
	done
	[[ $NFAIL -eq 0 ]] || diag "$NFAIL of $NTEST failed"
	exit $((NFAIL > 0))
}

# SIFT_NODES=docker: nodes are containers (scripts/replication.sh --container)
[[ ${SIFT_NODES:-} != docker ]] || . "$(dirname "${BASH_SOURCE[0]}")/lib_docker.bash"
