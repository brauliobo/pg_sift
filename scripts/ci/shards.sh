# SIFT_CI_SHARD=K/N: cuts the queue into the slice of this runner, prints the plan for --plan, and refuses a run with no job.
# The full tier cuts by weight (shard_loads), the quick tier takes every Nth job. Sourced by scripts/ci.sh; not a script of its own.
# SIFT_CI_SHARD=K/N: every Nth job of the queue from the Kth, so that N runners share the pool and each keeps the longest-first order; shard 1 runs
# the checks of the harness. Before PostgreSQL 18 the jobs that run after the pool (flavor installs, serial) are all the last shard's
# The full tier cuts the queue by weight instead (JOB_WEIGHT: seconds of a lane, the jobs of 90 s and more, the mean of the least times of a job on PostgreSQL 17 and 18 in
# the lean runs 40484446770, 40491216237 and 40494886743 of 2026-10-27; the others weigh 10; the column differential slices of a sift_table and the table form of diff_fuzz
# have no pass in those runs and keep their sizes of run 40097165396 at 1.15, the plan-gate lanes are half of their time with 3 timing runs): the heaviest job goes to the
# shard with the least load so far, which balances the shards to within a few percent (--selftest checks it, and that the shards are the plan exactly once). The deep sizes are
# about 1.5 to 2.5 times the lean ones on the same jobs, the same order.
# shard_loads N POOL: SHARD_OF[job] (0-based shard of the pool) and LOAD[k] for the jobs of the pool, heaviest first, ties in queue order
declare -A SHARD_OF
shard_loads() {
	local pool=$1 line id k i
	SHARD_OF=() LOAD=()
	for (( k = 0; k < pool; k++ )); do LOAD[k]=0; done
	for id in "${J_ID[@]}"; do [[ -v J_AFTER[$id] ]] && { weight_of "$id" "${SIFT_CI_DEEP:-}"; LOAD[pool - 1]=$(( LOAD[pool - 1] + WEIGHT )); }; done
	while read -r line; do
		id=${line#* }; k=0
		for (( i = 1; i < pool; i++ )); do (( LOAD[i] < LOAD[k] )) && k=$i; done
		SHARD_OF[$id]=$k LOAD[k]=$(( LOAD[k] + ${line% *} ))
	done < <(for id in "${J_ID[@]}"; do [[ -v J_AFTER[$id] ]] || { weight_of "$id" "${SIFT_CI_DEEP:-}"; echo "$WEIGHT $id"; }; done | sort -s -k1,1nr)
}
# --plan with SIFT_CI_SHARD=all/N (full tier): the plans of the N shards in one run, each line prefixed with the number of its shard (--selftest asks for them all)
if [[ -n $PLAN && $MODE == full && ${SIFT_CI_SHARD:-} =~ ^all/([0-9]+)$ ]]; then
	NSHARDS=${BASH_REMATCH[1]}
	shard_loads "$NSHARDS"
	for id in "${J_ID[@]}"; do
		if [[ -v J_AFTER[$id] ]]; then k=$NSHARDS where=after; else k=$(( SHARD_OF[$id] + 1 )) where=pool; fi
		echo "$k $id ${J_SUITE[$id]} $where ${J_CMD[$id]}"
	done
	exit 0
fi
SHARD=1
if [[ ${SIFT_CI_SHARD:-} =~ ^([0-9]+)/([0-9]+)$ ]]; then
	SHARD=${BASH_REMATCH[1]} NSHARDS=${BASH_REMATCH[2]} KEEP_IDS=() SUITES=() n=0
	POOL=$NSHARDS
	# the quick tier gives the jobs that run after the pool (flavor installs, serial: minutes long) a shard of their own; in the full tier they are a few lane minutes and end the last shard
	[[ $MODE == full ]] || for id in "${J_ID[@]}"; do [[ -v J_AFTER[$id] ]] && (( NSHARDS > 1 )) && { POOL=$(( NSHARDS - 1 )); break; }; done
	[[ $MODE == full && $NSHARDS -gt 1 ]] && shard_loads "$POOL"
	for id in "${J_ID[@]}"; do
		if [[ -v J_AFTER[$id] ]]; then (( NSHARDS > 1 && SHARD == NSHARDS )) || (( NSHARDS == 1 )) || continue
		elif [[ $MODE == full && $NSHARDS -gt 1 ]]; then (( SHARD_OF[$id] == SHARD - 1 )) || continue
		else (( n++ % POOL == SHARD - 1 )) || continue; fi
		KEEP_IDS+=("$id")
		[[ " ${SUITES[*]-} " == *" ${J_SUITE[$id]} "* ]] || SUITES+=("${J_SUITE[$id]}")
	done
	J_ID=("${KEEP_IDS[@]}")
fi
(( LANES <= ${#J_ID[@]} )) || LANES=$(( ${#J_ID[@]} > 0 ? ${#J_ID[@]} : 1 ))   # a shard with no job still builds
if [[ -n $PLAN ]]; then
	for id in "${J_ID[@]}"; do where=pool; [[ -v J_AFTER[$id] ]] && where=after; echo "$id ${J_SUITE[$id]} $where ${J_CMD[$id]}"; done   # no $(...) here: a fork for each of 400 jobs
	exit 0
fi
# a run that selects no job would build, run the harness and exit 0 with no test run (a shard may be empty and still builds)
(( ${#J_ID[@]} || ${NSHARDS:-1} > 1 )) || { echo "ci: no test job selected (SIFT_CI_JOBS='${SIFT_CI_JOBS:-}' SIFT_CI_AFFECTED='${SIFT_CI_AFFECTED:-}')" >&2; exit 1; }
