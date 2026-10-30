# How a lane takes a job: tidy after a pass, and the claim of the first job that fits the scratch budget under a lock directory.
# scripts/test-ci-claim.sh runs claim and claim_locked from this file. Sourced by scripts/ci.sh; not a script of its own.
# tidy ID: the data of a job that passed goes at once, so that the scratch directory holds only what runs: its databases in the
# lane's cluster, or its directory after the leak gate scanned it (a finding keeps the directory for the scan of the whole run)
tidy() {
	if [[ ${J_CLUSTER[$1]} == 1 ]]; then "$S/pgtest.sh" clean >/dev/null 2>&1
	else "$S/leakcheck.sh" scan "$SIFT_TMP/$1" >/dev/null 2>&1 && rm -rf "${SIFT_TMP:?}/$1"; fi
}

# claim: sets CLAIMED to the first job of the queue that nobody has claimed and whose estimate fits the budget with the ones
# running (the first job always fits); returns 1 when the queue is empty, 2 when nothing fits yet. The lock is a directory: macOS has no flock
# A lane that dies holding the lock would hang the others in this loop until the job timeout, so the lock names its owner (pid) and a waiter removes a lock whose owner is
# gone, or that has no owner after 60 s (a lane killed between the mkdir and the write of the pid)
claim() {
	local rc spin=0 owner stale=''
	until mkdir "$BASE/claim.lock" 2>/dev/null; do
		[[ -d $BASE ]] || { echo "ci: $BASE is gone, the lane stops" >&2; exit 1; }
		if (( ++spin % 50 == 0 )); then
			owner=$(cat "$BASE/claim.lock/pid" 2>/dev/null)
			if [[ -n $owner ]]; then kill -0 "$owner" 2>/dev/null || stale=1; else (( spin < 3000 )) || stale=1; fi
			if [[ -n $stale ]]; then
				echo "ci: $LANE removes the claim lock of ${owner:-no owner}, which is gone" >&2
				rm -rf "$BASE/claim.lock"
				stale=
			fi
		fi
		sleep 0.02
	done
	echo "$BASHPID" >"$BASE/claim.lock/pid"
	claim_locked && rc=0 || rc=$?
	rm -rf "$BASE/claim.lock"
	return $rc
}
claim_locked() {
	local id est used=0 left=0 f v
	# a lane removes its file from running/ without the lock, at any moment: a file that is gone (or empty) counts for nothing, and no error may leave this function with the lock held
	for f in "$BASE"/running/*; do
		v=0; { read -r v; } 2>/dev/null <"$f" || v=0
		used=$((used + ${v:-0}))
	done
	for id in "${J_ID[@]}"; do
		[[ ! -d $BASE/claims/$id && ! -v J_AFTER[$id] ]] || continue
		left=1
		est=${EST_MB[$id]:-$JOB_MB}
		(( used == 0 || used + est <= BUDGET_MB )) || continue
		mkdir "$BASE/claims/$id"
		echo "$est" >"$BASE/running/$id"
		echo "$LANE" >"$BASE/claims/$id/lane"
		echo "$SECONDS" >"$BASE/claims/$id/start"
		CLAIMED=$id
		return 0
	done
	return $(( left ? 2 : 1 ))
}
