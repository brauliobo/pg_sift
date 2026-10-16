# Shared helpers for the TAP-emitting shell tests in test/t (run them with
# `scripts/pgtest.sh tap`). Nodes live under $SIFT_TMP/tap/<name>, listen on
# unix sockets only and load sift from the staging root $SIFT_ROOT. Every
# node is stopped and its directory deleted when the script exits; logs of a
# failed run are kept. SIFT_PRELOAD (e.g. libasan.so for `make SANITIZE=1`)
# is preloaded into the servers only, and a sanitizer report in any server
# log fails the script.
set -uo pipefail
export LC_ALL=C

# knob VAR FULL QUICK: $VAR when set, else FULL, or QUICK under SIFT_QUICK=1 (scripts/ci.sh quick)
knob() { echo "${!1:-$([[ -n ${SIFT_QUICK:-} ]] && echo "$3" || echo "$2")}"; }

TAPDIR=${SIFT_TMP:?}/tap
ROOT=${SIFT_ROOT:?run through scripts/pgtest.sh tap}
SHAREDIR=$ROOT$(pg_config --sharedir)
PKGLIBDIR=$ROOT$(pg_config --pkglibdir)
PG_MAJOR=$(pg_config --version | sed -n 's/^PostgreSQL \([0-9][0-9]*\).*/\1/p')
PATHS_CONF=$("$(dirname "${BASH_SOURCE[0]}")/../../scripts/pgtest.sh" conf)
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
not_ok() { NTEST=$((NTEST + 1)); NFAIL=$((NFAIL + 1)); echo "not ok $NTEST - $*"; }
diag() { sed 's/^/# /' <<<"$*"; }
check() { local desc=$1; shift; if "$@"; then ok "$desc"; else not_ok "$desc"; fi; }
skip() { local n=$1; shift; for _ in $(seq "$n"); do NTEST=$((NTEST + 1)); echo "ok $NTEST # skip $*"; done; }
bail() { NFAIL=$((NFAIL + 1)); echo "Bail out! $*"; exit 1; }   # a bail keeps the node logs

dir() { echo "$TAPDIR/$1"; }
port() { echo $((SIFT_PORT + $(cat "$(dir "$1")/offset"))); }

# node_init NAME OFFSET [conf lines]: fresh cluster on SIFT_PORT + OFFSET
node_init() {
	local name=$1 offset=$2 d
	d=$(dir "$name")
	node_stop "$name"
	rm -rf "$d"
	mkdir -p "$d/sock"
	echo "$offset" >"$d/offset"
	NODES+=("$name")
	initdb -D "$d/data" -A trust -U postgres -E UTF8 --no-locale --no-instructions >"$d/initdb.log" 2>&1 ||
		bail "initdb $name failed"
	node_conf "$name" "port = $((SIFT_PORT + offset))
listen_addresses = ''
unix_socket_directories = '$d/sock'
$PATHS_CONF
fsync = off
max_wal_size = 256MB
restart_after_crash = off
shared_buffers = 128MB
max_connections = 60
${3:-}"
}

node_conf() { printf '%s\n' "$2" >>"$(dir "$1")/data/postgresql.conf"; }
node_start() {
	LD_PRELOAD=${SIFT_PRELOAD:-} pg_ctl -D "$(dir "$1")/data" -l "$(dir "$1")/postgres.log" -w -t 120 start \
		>/dev/null 2>&1
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

# kill -9 the postmaster and every child at once, as a power loss would; the postmaster is stopped
# first so that it cannot fork a parallel worker that would outlive the list of children
crash() {
	local pm kids
	pm=$(head -1 "$(dir "$1")/data/postmaster.pid")
	kill -STOP "$pm" 2>/dev/null
	while [[ $(cut -d' ' -f3 "/proc/$pm/stat" 2>/dev/null) =~ ^[RSD]$ ]]; do :; done
	kids=$(pgrep -P "$pm" | tr '\n' ' ')
	kill -9 "$pm" $kids 2>/dev/null
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

finish() {
	local name
	for name in "${NODES[@]}"; do
		! grep -qE "AddressSanitizer|runtime error:" "$(dir "$name")/postgres.log" 2>/dev/null ||
			{ not_ok "sanitizer report in $name's log"; diag "$(grep -A20 -m1 -E "AddressSanitizer|runtime error:" "$(dir "$name")/postgres.log")"; }
	done
	[[ $NFAIL -eq 0 ]] || diag "$NFAIL of $NTEST failed"
	exit $((NFAIL > 0))
}
