# Shared helpers for the TAP-emitting shell tests in test/t (test/t/lib/sql.bash and conc.bash add the common checks; run them with
# `scripts/pgtest.sh tap`). Nodes live under $SIFT_TMP/tap/<name>, listen on
# unix sockets only and load sift from the staging root $SIFT_ROOT. Every
# node is stopped and its directory deleted when the script exits; logs of a
# failed run are kept. SIFT_PRELOAD (e.g. libasan.so for `make SANITIZE=1`)
# is preloaded into the servers only. finish runs the leak gate of
# scripts/leakcheck.sh on every node (cluster invariants, clean shutdown, log
# scan: leak warnings, crashes, sanitizer and LeakSanitizer reports) and fails
# the script on a finding.
# SIFT_POOLED_CLUSTER=1 (scripts/ci.sh sets it for the scripts that say `# tap: pooled`): a node of one server that does not restart, crash, clone or read its
# data directory lives in a database of the cluster of the lane (pool_init) instead of a cluster of its own: no initdb copy, no start, no stop. A node that
# cannot (a setting that only a restart changes, WAL settings) is an ordinary node, and a pooled node that is crashed, stopped or cloned fails the script.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -uo pipefail
export LC_ALL=C
TAP_LIB=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TAP_SCRIPTS=$TAP_LIB/../../../scripts
. "$TAP_SCRIPTS/leakcheck.sh"

# timeout SECONDS COMMAND on a host without timeout(1) (macOS before 26)
command -v timeout >/dev/null || timeout() { local s=$1; shift; perl -e 'alarm shift; exec @ARGV' "$s" "$@"; }

# knob VAR FULL QUICK: $VAR when set, else FULL, or QUICK under SIFT_QUICK=1 (scripts/ci.sh quick)
knob() { echo "${!1:-$([[ -n ${SIFT_QUICK:-} ]] && echo "$3" || echo "$2")}"; }
# the largest segment the store churn writes: 2 MB (full), 500 KB quick (8 times less live data and file growth per node; still
# several times SIFT_ALLOC_MAX_LOCKED blocks, so the segments span extents; the 20 MB writes of the tests stay)
CHURN_BYTES=$(knob SIFT_CHURN_BYTES 2000000 500000)

TAPDIR=${SIFT_TMP:?}/tap
ROOT=${SIFT_ROOT:-}
[[ -n $ROOT || ${SIFT_NODES:-} == docker ]] || { echo "run through scripts/pgtest.sh tap" >&2; exit 1; }
SHAREDIR=$ROOT$(pg_config --sharedir 2>/dev/null)
PG_MAJOR=$(pg_config --version 2>/dev/null | sed -n 's/^PostgreSQL \([0-9][0-9]*\).*/\1/p')
PKGLIBDIR=$( (( PG_MAJOR < 18 )) || printf %s "$ROOT")$(pg_config --pkglibdir 2>/dev/null)
DLSUFFIX=.so; [[ $(uname) != Darwin ]] || DLSUFFIX=.dylib   # the suffix of a PostgreSQL module
NTEST=0
NFAIL=0
BAILED=0
PLANNED=
PLAN_OPEN=
NODES=()
declare -A POOL_DB=()   # the pooled nodes: name -> database of the cluster of the lane (pool_init)
# tap_script_id SCRIPT: SCRIPT as the identifier of its databases: its name under test/t when that is up to 32 letters, digits and underscores, else its path with 8 hex digits
# of the sha256 of the path (tap_<id>_<node> stays under the 63 bytes of an identifier, and two scripts of different directories never share one)
tap_script_id() {
	local path id
	path=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
	path=${path#"$(cd "$TAP_LIB/.." && pwd)"/}
	id=${path%.sh}
	[[ $id =~ ^[A-Za-z0-9_]{1,32}$ ]] && { echo "$id"; return; }
	id=${id//[^A-Za-z0-9_]/_}
	echo "${id:0:23}_$(printf %s "$path" | "$TAP_SCRIPTS/sha256.sh" | cut -c1-8)"
}
POOL_SCRIPT=$(tap_script_id "$0")
POOL_SOCK=${SIFT_TMP}/sock POOL_LOG=${SIFT_TMP}/postgres.log POOL_DATA=${SIFT_TMP}/data
trap cleanup EXIT

cleanup() {
	local name
	(( BAILED )) || plan_audit
	plan_close
	tap_tx_release_all
	pool_drop_all
	for name in "${NODES[@]}"; do
		pg_ctl -D "$(dir "$name")/data" -m immediate stop >/dev/null 2>&1
		if [[ $NFAIL -eq 0 ]]; then rm -rf "$(dir "$name")"; elif [[ -z ${SIFT_TAP_KEEP:-} ]]; then rm -rf "$(dir "$name")/data"; fi
	done
}

# Megabytes used by a node's directory (data, WAL, logs)
node_mb() { du -sm "$(dir "$1")" | cut -f1; }

# TAP output
# plan N: the tests the script means to run. The plan line comes last, after the leak checks of finish (plan_close), so that a check that fails there is a test of its own and not
# a test beyond the plan; a script that ran another number than N fails by that (plan_audit)
plan() { PLANNED=$1; PLAN_OPEN=1; }
# done_testing: a script with no fixed number of tests; its plan is the tests it ran
done_testing() { PLAN_OPEN=1; }
plan_audit() { [[ -z $PLANNED ]] || { (( NTEST == PLANNED )) || not_ok "the script ran $NTEST tests, planned $PLANNED"; PLANNED=; }; }
plan_close() { [[ -z $PLAN_OPEN ]] || { PLAN_OPEN=; echo "1..$NTEST"; }; }
ok() { NTEST=$((NTEST + 1)); echo "ok $NTEST - $*"; }
# a failed check shows the end of every node log: what the servers did around it is rarely there on a rerun
not_ok() {
	local n
	NTEST=$((NTEST + 1)); NFAIL=$((NFAIL + 1)); echo "not ok $NTEST - $*"
	for n in "${NODES[@]}"; do [[ -f $(dir "$n")/postgres.log ]] && diag "log of node $n:"$'\n'"$(tail -25 "$(dir "$n")/postgres.log" | cut -c1-240)"; done
	(( ${#POOL_DB[@]} == 0 )) || diag "log of the cluster of the lane:"$'\n'"$(tail -25 "$POOL_LOG" | cut -c1-240)"
	return 0
}
# wait_for COND SECONDS: 0 as soon as the shell condition COND holds, 1 when it still does not after SECONDS; the first polls come 10, 20 and 50 ms apart, then every 50 ms,
# and it does not sleep after success. A wait on an observable condition with a large cap, not a fixed budget: a loaded host only makes it longer. A short timeout literal needs a note (scripts/check-tap-timeouts.sh)
wait_for() {
	local i delay
	for ((i = 0; i < $2 * 20; i++)); do
		eval "$1" && return 0
		case $i in 0) delay=0.01 ;; 1) delay=0.02 ;; *) delay=0.05 ;; esac
		sleep "$delay"
	done
	eval "$1"
}
# wait_stable NODE SQL SECONDS: the background work has stopped changing what the query SQL returns: no sift worker and the same value for 3 polls
# in a row (progress stops; the target is checked by the test). SECONDS is a safety cap that a loaded host does not reach
wait_stable() {
	local node=$1 query=$2 last= same=0
	stable_poll() {
		local v
		v=$(sql "$node" "SELECT ($query), EXISTS (SELECT 1 FROM pg_stat_activity WHERE backend_type LIKE 'sift %')")
		[[ $v == *'|f' && $v == "$last" ]] && same=$((same + 1)) || same=0
		last=$v
		((same >= 3))
	}
	wait_for stable_poll "$3"
}
diag() { sed 's/^/# /' <<<"$*"; }
# slope_case LINE: a SLOPE line of scripts/mem-slope*.py as a comment (the job log of a passing test keeps comments, not its ok lines: the summary of scripts/ci.sh lists them) and as the result
slope_case() { diag "$1"; if [[ $1 == *" PASS" ]]; then ok "${1#SLOPE }"; else not_ok "${1#SLOPE }"; fi; }
# a failed check shows what the test had captured in $out: a hosted run lost the cause of one failure because its log said only which check failed
check() { local desc=$1; shift; if "$@"; then ok "$desc"; else not_ok "$desc"; [[ -z ${out:-} ]] || diag "what the check read (out):"$'\n'"$(head -n 40 <<<"$out" | cut -c1-240)"; fi; }
skip() { local n=$1; shift; for _ in $(seq "$n"); do NTEST=$((NTEST + 1)); echo "ok $NTEST # skip $*"; done; }
bail() { BAILED=1; NFAIL=$((NFAIL + 1)); echo "Bail out! $*"; exit 1; }   # a bail keeps the node logs
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
port() { if [[ -v POOL_DB[$1] ]]; then echo "$SIFT_PORT"; else echo $((SIFT_PORT + $(cat "$(dir "$1")/offset"))); fi; }
node_db() { echo "${POOL_DB[$1]:-postgres}"; }
node_sock() { if [[ -v POOL_DB[$1] ]]; then echo "$POOL_SOCK"; else echo "$(dir "$1")/sock"; fi; }

# real_node NAME WHAT: WHAT needs a server of its own; a pooled node fails the script with the way out
real_node() { [[ ! -v POOL_DB[$1] ]] || bail "$2 on the pooled node $1: a script that does it is no \`# tap: pooled\` script"; }
pool_psql() { psql -X -At -q -v ON_ERROR_STOP=1 -h "$POOL_SOCK" -p "$SIFT_PORT" -U postgres -d postgres "$@"; }
# pool_init NAME OFFSET [conf lines]: NAME is a database of the cluster of the lane when SIFT_POOLED_CLUSTER=1 and every setting of the conf is one a session may set
# (ALTER DATABASE) or the value the cluster has already; 1, and the caller makes a server of its own, when one is not. One psql session decides and does it all
# (a conf that does not fit creates nothing)
pool_init() {
	local db=tap_${POOL_SCRIPT}_$1 conf=${3:-} line k v want= out
	[[ -n ${SIFT_POOLED_CLUSTER:-} && $conf != "$WAL_CONF"* && -S $POOL_SOCK/.s.PGSQL.$SIFT_PORT ]] || return 1
	while IFS= read -r line; do
		[[ $line == *' = '* ]] || continue
		k=${line%% = *} v=${line#* = }
		v=${v#\'} v=${v%\'}
		want+="${want:+, }('${k//\'/\'\'}', '${v//\'/\'\'}')"
	done <<<"$conf"
	local misfit=false alter="SELECT 1 WHERE false"
	if [[ -n $want ]]; then
		misfit="EXISTS (WITH want(k, v) AS (VALUES $want) SELECT 1 FROM want w LEFT JOIN pg_settings s ON s.name = w.k
			WHERE NOT (coalesce(s.context IN ('user', 'superuser'), false) OR coalesce(lower(current_setting(w.k, true)) = lower(w.v), false)))"
		alter="WITH want(k, v) AS (VALUES $want) SELECT format('ALTER DATABASE $db SET %s = %L', w.k, w.v) FROM want w JOIN pg_settings s ON s.name = w.k
			WHERE s.context IN ('user', 'superuser')"
	fi
	out=$(pool_psql <<-SQL
		LOAD 'pg_sift';
		SELECT $misfit AS misfit \gset
		\if :misfit
			\echo MISFIT
		\else
			DROP DATABASE IF EXISTS $db WITH (FORCE);
			CREATE DATABASE $db;
			$alter \gexec
		\endif
	SQL
	) || return 1
	[[ $out != MISFIT ]] || return 1
	mkdir -p "$(dir "$1")"
	leak_before "tap-$POOL_SCRIPT-$1" "$POOL_LOG"
	POOL_DB[$1]=$db
	diag "node $1 is the database $db of the cluster of the lane"
}
# pool_end NAME: the leak check of the work of the pooled node (the server log since pool_init, the backends of its database), then the database goes
pool_end() {
	local rc=0
	leak_after "tap-$POOL_SCRIPT-$1" "$POOL_DATA" "${POOL_DB[$1]}" || rc=1
	pool_psql -c "DROP DATABASE IF EXISTS ${POOL_DB[$1]} WITH (FORCE)" >/dev/null || rc=1
	unset "POOL_DB[$1]"
	return $rc
}
pool_drop_all() {
	local name
	for name in "${!POOL_DB[@]}"; do pool_psql -c "DROP DATABASE IF EXISTS ${POOL_DB[$name]} WITH (FORCE)" >/dev/null 2>&1; [[ $NFAIL -ne 0 ]] || rm -rf "$(dir "$name")"; done
}

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
	pool_init "$@" && return
	[[ ${3:-} != "$WAL_CONF"* ]] || kind=wal
	d=$(dir "$name")
	node_stop "$name"
	rm -rf "$d"
	mkdir -p "$d/sock"
	echo "$offset" >"$d/offset"
	NODES+=("$name")
	"$TAP_SCRIPTS/pgtest.sh" new-data "$d/data" $kind >"$d/initdb.log" 2>&1 || bail "initdb $name failed"
	node_conf "$name" "port = $((SIFT_PORT + offset))
listen_addresses = ''
unix_socket_directories = '$d/sock'
$("$TAP_SCRIPTS/pgtest.sh" conf)
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
	real_node "$1" node_clone
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

node_promote() { real_node "$1" node_promote; pg_ctl -D "$(dir "$1")/data" -w promote >/dev/null; }

# libpq connection string of a node, for CREATE SUBSCRIPTION
conninfo() { echo "host=$(dir "$1")/sock port=$(port "$1") user=postgres dbname=postgres"; }

node_conf() { real_node "$1" node_conf; printf '%s\n' "$2" >>"$(dir "$1")/data/postgresql.conf"; }
# use_root NODE ROOT: NODE loads the sift installed under ROOT (a released library built by scripts/old-lib-check.sh, or $newroot, the one of this
# tree). Only this tree defines the sift. settings scripts/pgtest.sh conf sets: a released library warns that "sift" is a reserved prefix in the first statement of a session, and the test
# compares what the statements print.
declare -gA NODE_ROOT
use_root() {
	local conf=$(dir "$1")/data/postgresql.conf tree k
	real_node "$1" use_root
	ROOT=$2
	NODE_ROOT[$1]=$2
	tree=$("$TAP_SCRIPTS/pgtest.sh" conf | grep -E '^sift\.[a-z0-9_]+ = ')
	while IFS=' ' read -r k _; do sed_i "/^${k//./\\.} = /d" "$conf"; done <<<"$tree"
	[[ $ROOT != "$newroot" ]] || { printf '%s\n' "$tree" >>"$conf"; sed_i '/^sift\.scope_max_multiplicity = /d' "$conf"; }
	node_conf "$1" "extension_control_path = '$ROOT$(pg_config --sharedir):\$system'
dynamic_library_path = '$ROOT$(pg_config --pkglibdir):\$libdir'"
}
# start_failed NODE: the tail of the log of a server that did not start (a test keeps what it prints, not the data directory)
start_failed() { diag "the server of $1 did not start; the end of its log:"$'\n'"$(tail -n 30 "$(dir "$1")/postgres.log" 2>&1 | cut -c1-240)"; return 1; }
node_start() {
	[[ ! -v POOL_DB[$1] ]] || return 0
	LD_PRELOAD=${SIFT_PRELOAD:-} pg_ctl -D "$(dir "$1")/data" -l "$(dir "$1")/postgres.log" -w -t 120 start \
		>/dev/null 2>&1 || start_failed "$1" || return
	leak_baseline "$(dir "$1")/data"
}
node_stop() { real_node "$1" node_stop; [[ -f $(dir "$1")/data/postmaster.pid ]] && pg_ctl -D "$(dir "$1")/data" -m fast -w stop >/dev/null 2>&1; true; }

# sql NAME [QUERY...]: unaligned tuples, stops at the first error (stderr
# kept); without queries, runs the script on stdin
sql() {
	local name=$1
	shift
	if [[ -v POOL_DB[$name] ]]; then
		PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$POOL_SOCK" -p "$SIFT_PORT" -U postgres -d "${POOL_DB[$name]}" "${@/#/--command=}"
		return
	fi
	PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir "$name")/sock" \
		-p "$(port "$name")" -U postgres -d postgres "${@/#/--command=}"
}

# sess_open NODE NAME: a psql session of NODE that reads its statements from a FIFO (sess_say NAME STATEMENTS), the output in <node directory>/NAME.out; sess_close NAME ends it and waits for it
declare -A SESS_FD=() SESS_PID=() SESS_DIR=()
sess_open() {
	local d=$(dir "$1") fd other
	mkfifo "$d/$2.in"
	(for other in "${SESS_FD[@]}"; do exec {other}>&-; done
	 exec psql -X -At -q -h "$d/sock" -p "$(port "$1")" -U postgres -d postgres <"$d/$2.in" >"$d/$2.out" 2>&1) &
	SESS_PID[$2]=$!
	SESS_DIR[$2]=$d
	exec {fd}>"$d/$2.in"
	SESS_FD[$2]=$fd
}
sess_say() { echo "${*:2}" >&"${SESS_FD[$1]}"; }
sess_close() { local fd=${SESS_FD[$1]}; exec {fd}>&-; wait "${SESS_PID[$1]}"; rm -f "${SESS_DIR[$1]}/$1.in"; }
sess_count() { sql "$1" "SELECT count(*) FROM pg_stat_activity WHERE application_name = '$2' AND $3"; }
# race NODE FIRST SECOND [END]: a session opens a transaction that runs FIRST, another runs SECOND, which waits for it or fails at once, and the first ends it with END (COMMIT);
# RACE_WAITED is 1 when the second waited on a lock; the outputs are <node directory>/ra.out and rb.out
race() {
	local d=$(dir "$1")
	sess_open "$1" ra; sess_open "$1" rb
	sess_say ra "SET application_name = 'ra'; BEGIN; $2; \\echo done"
	sess_say rb "SET application_name = 'rb';"
	wait_for "grep -q '^done\$' $d/ra.out && [[ \$(sess_count $1 rb 'true') == 1 ]]" 60
	sess_say rb "$3; \\echo done"
	wait_for "[[ \$(sess_count $1 rb \"wait_event_type = 'Lock'\") == 1 ]] || grep -q '^done\$' $d/rb.out" 60
	RACE_WAITED=$(sess_count "$1" rb "wait_event_type = 'Lock'")
	sess_say ra "${4:-COMMIT};"
	sess_close ra; sess_close rb
}

# The groups of the elements of orders.doc->'items' (the table of the sift_col tests) per category: from the column source and its
# pre-aggregates ($2 = on) or from the normal plan ($2 = off), as one string; $3 = explain: the units the pre-aggregates answered
ELEM_QUERY="SELECT e.category, count(*), sum(e.qty) FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.doc->'items') e(category text, qty int) GROUP BY 1"
elem_groups() {
	local run="SELECT coalesce(string_agg(x::text, ',' ORDER BY x::text), '') FROM ($ELEM_QUERY) x"
	[[ ${3:-} == explain ]] && run="EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $ELEM_QUERY"
	sql "$1" "LOAD 'pg_sift'" "SET sift.enable_agg = $2" "SET sift.agg_preagg = $2" "SET max_parallel_workers_per_gather = 0" \
		"SET seq_page_cost = 10000" "$run" | tail -n +1 | grep -v '^$' | { [[ ${3:-} == explain ]] && grep -oE '[0-9]+ from pre-aggregates' || cat; }
}

# kill -9 the postmaster and every child at once, as a power loss would; the postmaster is stopped
# first so that it cannot fork a parallel worker that would outlive the list of children. Stop every child before
# killing the postmaster: a running parallel leader otherwise observes its death and enters fatal-error cleanup.
crash() {
	local pm kids pid state
	real_node "$1" crash
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
	wait_for "[[ \$(sql $2 \"SELECT pg_last_wal_replay_lsn() >= '$lsn'\") == t ]]" 60
}
# standby_setting STANDBY GUC VALUE: a new session of the standby reads VALUE for GUC (the reload has reached it)
standby_setting() { wait_for "[[ \$(sql $1 \"SHOW $2\") == '$3' ]]" 30; }

# running NODE PATTERN: another backend of NODE is running a statement that matches the LIKE pattern PATTERN
running() { [[ $(sql "$1" "SELECT count(*) FROM pg_stat_activity WHERE state = 'active' AND pid <> pg_backend_pid() AND query LIKE '$2'") -ge 1 ]]; }
# wal_flushed NODE: the WAL writer has flushed every record written so far, which is what a crash after it keeps
wal_flushed() { [[ $(sql "$1" "SELECT pg_current_wal_flush_lsn() = pg_current_wal_insert_lsn()") == t ]]; }
# setting_is NODE NAME VALUE: a new session of NODE sees the setting NAME as VALUE (a reload has reached it)
setting_is() { [[ $(sql "$1" "SHOW $2") == "$3" ]]; }
# pgbench_up NODE CLIENTS: the CLIENTS sessions of the pgbench of NODE are connected
pgbench_up() { wait_for "[[ \$(sql $1 \"SELECT count(*) FROM pg_stat_activity WHERE application_name = 'pgbench'\") -ge $2 ]]" 30; }
# horizon_past PRIMARY STANDBY: the standby replayed what PRIMARY wrote and its feedback (hot_standby_feedback) holds no horizon below the next transaction id of PRIMARY: the slot of the standby, which is named after its node, has an xmin at it or past it, or none
horizon_past() {
	local next
	next=$(sql "$1" "SELECT pg_snapshot_xmin(pg_current_snapshot())::text::int8")
	catchup "$1" "$2" && wait_for "[[ \$(sql $1 \"SELECT coalesce(xmin::text::int8 >= $next, true) FROM pg_replication_slots WHERE slot_name = '$2'\") == t ]]" 30
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
	pgbench -n -h "$(node_sock "$name")" -p "$(port "$name")" -U postgres -c "$clients" -j 4 -T "$secs" "$@" \
		"$(node_db "$name")" >"$(dir "$name")/pgbench.log" 2>&1 &
	PGBENCH_PID=$!
}

# crash_cycle NAME LABEL MAXSLEEP CLIENTS ARGS...: pgbench (600 s at most) under CLIENTS clients, killed with the cluster after a random delay
# of up to MAXSLEEP seconds, then the restart; false (not ok LABEL, the log) when the node does not come back. The delay counts from the first
# work of the clients when CRASH_READY names a query that returns t once they have done some (pgbench connects its clients before it runs
# anything, which on a busy host outlasts the delay and kills a cluster that has done nothing): false, with the log of pgbench, when that
# does not happen within 120 s
crash_cycle() {
	local name=$1 label=$2 max=$3 clients=$4 w ready=
	shift 4
	pgbench_bg "$name" "$clients" 600 "$@"
	if [[ -n ${CRASH_READY:-} ]]; then
		for w in $(seq 240); do [[ $(sql "$name" "$CRASH_READY" 2>/dev/null) == t ]] && { ready=1; break; }; sleep 0.5; done
		if [[ -z $ready ]]; then
			crash "$name"
			wait "$PGBENCH_PID"
			not_ok "$label: the clients did no work within 120 s"
			diag "$(tail -5 "$(dir "$name")/pgbench.log")"
			return 1
		fi
	fi
	sleep "$((RANDOM % max)).$((RANDOM % 9 + 1))$((RANDOM % 10))"
	crash "$name"
	wait "$PGBENCH_PID"
	node_start "$name" && return 0
	not_ok "$label: restart"
	diag "$(tail -20 "$(dir "$name")/postgres.log")"
	return 1
}

# node_stop_fast DATA: the fast shutdown the leak gate asks for. One that does not end in 120 s is a finding of its own, with what each
# process of the cluster waits for (the immediate stop that follows leaves temporary files that are not the finding)
node_stop_fast() {
	local pid
	pg_ctl -D "$1" -m fast -w -t 120 stop >/dev/null 2>&1 && return 0
	pid=$(head -1 "$1/postmaster.pid" 2>/dev/null)
	leak_finding "fast shutdown did not end in 120 s; the processes left:"
	ps -o pid,ppid,stat,wchan:30,args --ppid "${pid:-0}" 2>&1 | sed 's/^/# /' >&2
	for pid in $(pgrep -P "${pid:-0}"); do
		command -v gdb >/dev/null && { echo "# backtrace of $pid:"; gdb -p "$pid" -batch -ex bt 2>&1 | tail -n 30; } >&2
	done
	pg_ctl -D "$1" -m immediate stop >/dev/null 2>&1
	return 1
}

# node_leakcheck NAME: the cluster invariants while it runs, a clean shutdown (LeakSanitizer reports as the processes exit)
# and the scan of its log
node_leakcheck() {
	local d
	d=$(dir "$1")
	if [[ -f $d/data/postmaster.pid ]]; then
		PGHOST=$d/sock PGPORT=$(port "$1") PGUSER=postgres leak_end "$d/data" "$d" node_stop_fast "$d/data"
	else
		leak_scan "$d"
	fi
}

# upgrade_failed NODE: after a failed pg_upgrade into NODE, the tail of its log and the error lines of the logs under pg_upgrade_output.d (a restore command is
# echoed whole after its error, so the tail of a dump log is the end of the statement, not the error; loadable_libraries.txt when the library check failed); the logs are copied next to pg_upgrade.log, which outlives the data directory
upgrade_failed() {
	local d logs
	d=$(dir "$1"); logs=$(ls -d "$d"/data/pg_upgrade_output.d/*/log 2>/dev/null | tail -n 1)
	[[ -z $logs ]] || cp -r "$logs" "$d/upgrade-logs"
	diag "$(tail -n 20 "$d/pg_upgrade.log"; cat "$d"/data/pg_upgrade_output.d/*/loadable_libraries.txt 2>/dev/null; grep -hE -A2 'ERROR|error:|TRAP|FATAL|PANIC' "$d"/upgrade-logs/*.log 2>/dev/null | head -n 40)"
}

finish() {
	local name out
	plan_audit
	for name in "${!POOL_DB[@]}"; do
		out=$(pool_end "$name" 2>&1) || { not_ok "leak check of node $name"; diag "$out"; }
	done
	for name in "${NODES[@]}"; do
		out=$(node_leakcheck "$name" 2>&1) || { not_ok "leak check of node $name"; diag "$out"; }
	done
	plan_close
	[[ $NFAIL -eq 0 ]] || diag "$NFAIL of $NTEST failed"
	exit $((NFAIL > 0))
}

. "$TAP_LIB/sql.bash"
. "$TAP_LIB/conc.bash"

# SIFT_NODES=docker: nodes are containers (scripts/replication.sh --container)
[[ ${SIFT_NODES:-} != docker ]] || . "$TAP_LIB/docker.bash"
