#!/usr/bin/env bash
# The bytes-per-row gate (scripts/bpr/run.py, cases.py, fixtures.sql): the disk and the WAL a row costs on each storage path of sift, in windows that start after a CHECKPOINT of a
# cluster with full_page_writes on and wal_compression off, so that a bulk load, a build, a write or a VACUUM that costs 3.5 times what it did fails here even when its ratio to
# the last run does not. One BPR line a case (a comment, as the job log of a passing test keeps comments) and one check each. A case that names a known defect (open=W-NN)
# is a TODO while it fails its ceilings and fails when it stops: its row is then made clean. BPR_ONLY=RE runs the cases whose name matches, BPR_ARGS=--calibrate (--determinism, --check-headroom) the other modes of the runner. The cases of wal_level = minimal run
# on a second cluster; the cases that restart the server (cold) restart the one they run on.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
REPO=${SIFT_REPO:?}

CONF="shared_preload_libraries = 'pg_sift'
full_page_writes = on
wal_compression = off
autovacuum = off
sift.async_maintenance = off
checkpoint_timeout = 1h
max_wal_size = 4GB
min_wal_size = 1GB
wal_writer_delay = 10ms
max_parallel_maintenance_workers = 0
max_parallel_workers_per_gather = 0
maintenance_work_mem = 16MB
work_mem = 4MB"

node_init bpr 1 "$CONF
wal_level = replica"
node_init bprmin 2 "$CONF
wal_level = minimal
max_wal_senders = 0"
node_start bpr || bail "start failed"
node_start bprmin || bail "start failed"

restart() {
	local d=$(dir "$1")
	echo "pg_ctl -D '$d/data' -m fast -w stop && LD_PRELOAD='${SIFT_PRELOAD:-}' pg_ctl -D '$d/data' -l '$d/postgres.log' -w -t 120 start && '$TAP_SCRIPTS/leakcheck.sh' baseline '$d/data'"
}
bpr() {
	local node=$1
	shift
	PGHOST=$(dir "$node")/sock PGPORT=$(port "$node") PGUSER=postgres PGDATABASE=postgres BPR_RESTART=$(restart "$node") \
		python3 -I "$REPO/scripts/bpr/run.py" --dir "$(dir "$node")" ${BPR_ONLY:+--only "$BPR_ONLY"} ${BPR_ARGS:-} "$@"
}
# measure NODE LEVEL: the runner of the LEVEL cases on NODE, its output, errors and status in the directory of the node. The two clusters measure side by side: each counts
# the WAL and the disk of its own server, so the other's load moves no byte of its windows
measure() {
	local d
	d=$(dir "$1")
	bpr "$1" --wal-level "$2" >"$d/bpr.out" 2>"$d/bpr.err"
	echo $? >"$d/bpr.rc"
}
report() {
	local node=$1 level=$2 d out rc
	d=$(dir "$node")
	out=$(<"$d/bpr.out") rc=$(<"$d/bpr.rc")
	[[ -s $d/bpr.err ]] && diag "$(cat "$d/bpr.err")"
	diag "$out"
	check "the runner of the $level cases exited 0 (it exited $rc)" test "$rc" -eq 0
	check "the runner ran every $level case" test "$(grep -c '^BPR [a-z]' <<<"$out")" -eq "$(bpr "$node" --wal-level "$level" --list | grep -c .)"
	while IFS= read -r l; do
		[[ $l == "BPR "[a-z]* ]] || continue
		name=${l#BPR } name=${name%% *} verdict=${l##* }
		case $verdict in
		PASS) ok "$name" ;;
		SKIP) ok "$name # SKIP release build" ;;
		OPEN=*) NTEST=$((NTEST + 1)); echo "not ok $NTEST - $name # TODO ${verdict#OPEN=}" ;;
		*) not_ok "$name: $l" ;;
		esac
	done <<<"$out"
}

done_testing
measure bpr replica & replica=$!
measure bprmin minimal & minimal=$!
wait "$replica" "$minimal"
report bpr replica
report bprmin minimal
finish
