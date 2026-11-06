# SIFT_CI_SHARD=K/N: cuts the queue into the slice of this runner, prints the plan for --plan, and refuses a run with no job. Sourced by scripts/ci.sh; not a script of its own.
# The jobs are cut by weight (weights.sh: the seconds of a lane each took in a hosted run): the heaviest job goes to the shard with the least load so far, which balances the shards to
# within a few percent (--selftest checks it, and that the shards are the plan exactly once). Shard 1 runs the checks of the harness. Before PostgreSQL 18 the jobs that run after the
# pool (flavor installs, serial) are all the last shard's; the quick tier gives them a shard of their own.
# shard_loads N POOL: SHARD_OF[job] (0-based shard of the pool) and LOAD[k] for the jobs of the pool, heaviest first, ties in queue order
declare -A SHARD_OF
shard_loads() {
	local pool=$1 line id k i
	SHARD_OF=() LOAD=()
	for (( k = 0; k < pool; k++ )); do LOAD[k]=0; done
	[[ $MODE == full ]] && for id in "${J_ID[@]}"; do [[ -v J_AFTER[$id] ]] && { weight_of "$id" "${SIFT_CI_DEEP:-}"; LOAD[pool - 1]=$(( LOAD[pool - 1] + WEIGHT )); }; done
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
	SHARD=${BASH_REMATCH[1]} NSHARDS=${BASH_REMATCH[2]} KEEP_IDS=() SUITES=()
	POOL=$NSHARDS
	# the quick tier gives the jobs that run after the pool (flavor installs, serial: minutes long) a shard of their own; in the full tier they are a few lane minutes and end the last shard
	[[ $MODE == full ]] || for id in "${J_ID[@]}"; do [[ -v J_AFTER[$id] ]] && (( NSHARDS > 1 )) && { POOL=$(( NSHARDS - 1 )); break; }; done
	(( NSHARDS > 1 )) && shard_loads "$POOL"
	for id in "${J_ID[@]}"; do
		if [[ -v J_AFTER[$id] ]]; then (( NSHARDS > 1 && SHARD == NSHARDS )) || (( NSHARDS == 1 )) || continue
		else (( NSHARDS == 1 || SHARD_OF[$id] == SHARD - 1 )) || continue; fi
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
