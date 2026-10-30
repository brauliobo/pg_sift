# A lane: run_job runs one job with its log, lane() takes jobs from the queue until it is empty and owns the cluster of the lane.
# Sourced by scripts/ci.sh; not a script of its own.
# run_job ID: the job with its output in logs/ID.log (a job without the lane's cluster gets a directory of its own)
run_job() {
	step "$1" "${J_SUITE[$1]}" bash -c '[[ $1 == 1 ]] || { SIFT_TMP=$SIFT_TMP/$2; mkdir -p "$SIFT_TMP"; export SIFT_TMP; }; eval "$3"' \
		_ "${J_CLUSTER[$1]}" "$1" "${J_CMD[$1]}"
}

# lane N: takes the jobs the budget admits, in queue order; its cluster starts with the first job that needs one
lane() {
	local id started='' rc
	LANE=lane$1
	export SIFT_TMP=$BASE/$LANE SIFT_PORT=$((BASE_PORT + 10 * ($1 - 1)))
	mkdir -p "$SIFT_TMP"
	while kill -0 "$MAIN" 2>/dev/null; do
		claim && rc=0 || rc=$?
		case $rc in 1) break ;; 2) sleep 0.5; continue ;; esac
		id=$CLAIMED
		if [[ ${J_CLUSTER[$id]} == 1 && -z $started ]]; then started=1; "$S/pgtest.sh" start >"$LOG/$LANE.start.log" 2>&1; fi
		run_job "$id" && tidy "$id"
		rm -f "$BASE/running/$id"
	done
	[[ -z $started ]] || "$S/pgtest.sh" stop
}
