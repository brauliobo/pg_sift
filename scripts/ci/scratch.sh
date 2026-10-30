# The scratch directory of a run: paths and ports, the watchdog on its size, the heartbeat, the cleanup on exit, and step().
# scripts/test-ci-claim.sh runs watchdog from this file. Sourced by scripts/ci.sh; not a script of its own.
# not /dev/shm by choice: the run needs about a gigabyte and fsync is off, so RAM buys nothing
# a Unix-domain socket path holds 103 bytes on macOS, where $TMPDIR is a long per-user directory: the clusters' sockets would not fit under it
TMPBASE=${TMPDIR:-/tmp}; [[ $(uname -s) != Darwin ]] || TMPBASE=/tmp
# macOS: initdb rejects LC_MESSAGES=C (below) next to an unset LANG, which a shell without a terminal (ssh, cron) has
[[ $(uname -s) != Darwin ]] || export LANG=${LANG:-en_US.UTF-8}
BASE=${SIFT_CI_TMP:-$TMPBASE/sift-ci-$$}
LOG=$BASE/logs
BASE_PORT=${SIFT_PORT:-56130}
# one initdb per run: every cluster starts as a copy (the shell tests and lane clusters, with 1 MB WAL segments; the
# PostgreSQL::Test::Cluster nodes, which are all WAL tests)
export SIFT_ROOT=$BASE/root SIFT_INITDB_TEMPLATE=$BASE/initdb-shell INITDB_TEMPLATE=$BASE/initdb-perl PG_TEST_PORT_DIR=$BASE/ports
reap_stale "$TMPBASE"
[[ -n ${CI:-} ]] || "$S/pressure.sh" --wait || echo "ci: the box is still under pressure after the wait, starting anyway" >&2   # a shared box: the lanes would stall the others
rm -rf "$BASE" && mkdir -p "$LOG" "$BASE/claims" "$BASE/running" "$PG_TEST_PORT_DIR"
t0=$SECONDS

watchdog() { # stops every postmaster under $BASE once the scratch directory passes the cap
	local peak=0 mb
	while sleep 2 && kill -0 "$MAIN" 2>/dev/null; do   # the lanes and these loops end with the run, also when it was killed without a trap
		mb=$(( $(du -sk "$BASE" 2>/dev/null | cut -f1) / 1024 ))
		(( mb > peak )) && { peak=$mb; echo "$peak" >"$LOG/peak_mb"; }
		if (( mb > MAX_MB )); then
			echo "disk cap exceeded: $mb MB > $MAX_MB MB (CI_MAX_MB)" >"$LOG/cap_exceeded"
			find "$BASE" -name postmaster.pid -exec head -1 {} \; | xargs -r kill -QUIT 2>/dev/null
			return
		fi
	done
}
watchdog &
WATCHDOG=$!
# a job that runs long (difftest-col takes two to three hours in the full tier) is silent in the live log until it ends: every 5 minutes the jobs past
# 10 minutes are listed with the last line of their own log, so that a long run is told from a hang by whether that line moves
heartbeat() {
	local f id s
	while sleep 300 && kill -0 "$MAIN" 2>/dev/null; do
		for f in "$BASE"/running/*; do
			[[ -e $f ]] || continue
			id=${f##*/}
			[[ -f $BASE/claims/$id/start ]] || continue
			s=$(<"$BASE/claims/$id/start")
			(( SECONDS - ${s:-$SECONDS} >= 600 )) && printf 'ci: running %s %s s (%s): %s\n' "$id" "$((SECONDS - ${s:-$SECONDS}))" "$(<"$BASE/claims/$id/lane")" "$(tail -n 1 "$LOG/$id.log" 2>/dev/null | cut -c1-160)"
		done
	done
}
heartbeat &
HEARTBEAT=$!
# SIFT_CI_STATS=1: the footprint of the run by component, lane and memory (scripts/ci_stats.sh), printed after the summary
[[ -z ${SIFT_CI_STATS:-} ]] || { "$S/ci_stats.sh" "$BASE" >"$LOG/stats.txt" 2>&1 & STATS=$!; }
# leaves nothing running: the lanes, their jobs and every postmaster under $BASE are stopped. The scratch directory goes when everything passed or the run was interrupted in
# a directory of its own; a failure keeps it for the logs (and `kept` tells reap_stale), as does an explicit SIFT_CI_TMP that a cancelled job uploads
cleanup() {
	trap '' INT TERM
	kill "$WATCHDOG" "$HEARTBEAT" ${STATS:-} 2>/dev/null
	local pids; pids=$(descendants $$)
	[[ -z $pids ]] || kill $pids 2>/dev/null
	stop_postmasters "$BASE"
	if [[ -z ${KEEP:-} ]] && { [[ ${failed:-1} == 0 ]] || [[ -n ${interrupted:-} && -z ${SIFT_CI_TMP:-} ]]; }; then rm -rf "$BASE"; else touch "$BASE/kept" 2>/dev/null; echo "scratch kept: $BASE"; fi
}
trap cleanup EXIT
trap 'interrupted=1; exit 130' INT
trap 'interrupted=1; exit 143' TERM

# lane_cpu ID: the CPU seconds the lane cluster's postmaster has spent on the backends and workers it reaped (a job on the lane cluster
# runs its queries there; the other jobs start clusters of their own, which scripts/cpu_time.py counts)
lane_cpu() {
	[[ ${J_CLUSTER[$1]:-0} == 1 && -f $SIFT_TMP/data/postmaster.pid ]] || { echo 0; return; }
	awk '{ print ($16 + $17) / 100 }' "/proc/$(head -1 "$SIFT_TMP/data/postmaster.pid")/stat" 2>/dev/null || echo 0
}
# step ID SUITE CMD...: runs one job or stage with its output in logs/ID.log; appends "id suite status seconds skips lane start end cpu_seconds"
# to the results and returns its status
step() {
	local id=$1 suite=$2 t=$SECONDS status=PASS skips c0 c1 rc=0
	shift 2
	[[ -e $LOG/cap_exceeded ]] && return
	c0=$(lane_cpu "$id")
	${JOB_TIMEOUT[@]+"${JOB_TIMEOUT[@]}"} "$S/cpu_time.py" "$LOG/$id.cpu" "$@" >"$LOG/$id.log" 2>&1 || { rc=$?; status=FAIL; }
	if (( rc == 124 || rc == 137 )); then
		{ echo "ci: $id stopped after $((SECONDS - t)) s (CI_JOB_TIMEOUT), the end of its log:"; tail -n 40 "$LOG/$id.log"; } | tee -a "$LOG/$id.log"
	fi
	c1=$(lane_cpu "$id")
	skips=$(grep -ciE '\bskip' "$LOG/$id.log" || true)
	echo "ci: $status $id $((SECONDS - t)) s ($LANE)"
	printf '%s %s %s %s %s %s %s %s %s\n' "$id" "$suite" "$status" $((SECONDS - t)) "$skips" "$LANE" "$t" "$SECONDS" \
		"$(awk -v a="$(cat "$LOG/$id.cpu" 2>/dev/null || echo 0)" -v c0="$c0" -v c1="$c1" 'BEGIN { printf "%.1f", a + c1 - c0 }')" >>"$LOG/results"
	# the log of a run that is cut (a timeout, a hang) names the jobs that had finished: the results are in the logs artifact of a failed run only
	printf 'ci: %-44s %s %5ss lane %s\n' "$id" "$status" $((SECONDS - t)) "$LANE"
	[[ $status == PASS ]]
}
