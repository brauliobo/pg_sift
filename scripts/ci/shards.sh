# shellcheck shell=bash
# SIFT_CI_SHARD=K/N: cuts the queue into the slice of this runner, prints the plan for --plan, and refuses a run with no job. Sourced by scripts/ci.sh; not a script of its own.
# The jobs are cut by weight (weights.sh: the seconds of a lane each took in a hosted run): the heaviest job goes to the shard with the least load so far, which balances the shards to
# within a few percent (--selftest checks it, and that the shards are the plan exactly once). Shard 1 runs the checks of the harness. Before PostgreSQL 18 the jobs that run after the
# pool (flavor installs, serial) are all the last shard's, which starts the pool with their weight already on it, times the lanes of a hosted runner (AFTER_LANES: they run alone, every
# second of theirs idles the other lanes). A shard queues its jobs heaviest first (heaviest_first): a lane that
# takes a 6 minute job last ends the shard 6 minutes after the others.
# The jobs that build older releases (the scripts of test/t that call build_old or scripts/old-lib-check.sh, and pg_upgrade) all go to the first shard: the workflow caches the built releases
# for each shard, so a job that moved to another shard when the weights changed built them cold (compat_matrix 721 s and old_lib_refusal 525 s beside 190 and 79 warm, hosted run 42142749980)
pinned() { # ID: whether the job is one of them. Every job that compiles is the first shard's, so that the others install nothing but the server
	local n
	! needs_toolchain "$1" || return 0
	[[ -n ${OLDLIB_TESTS:-} ]] || OLDLIB_TESTS=" $(grep -rl --include='*.sh' -E 'build_old|old-lib-check' "$REPO/test/t" 2>/dev/null | sed "s|^$REPO/test/t/||; s|\.sh$||" | tr '\n' ' ')compat/pg_upgrade/basic "
	for n in $OLDLIB_TESTS; do [[ $1 == "tap-shell-$n" || $1 == "tap-shell-${n}_"* ]] && return 0; done
	return 1
}
# The CPU-bound jobs (the differential tests, the hardened build, the fuzzers, the release library run, the scaling slope) run 1.5 to 2 times their weight when several share the 4 cores of a
# runner: in a hosted run the shard with four of them took 3568 lane seconds where its weights said 4600 and the shard with none 2082. They are spread first, to the shard with the fewest of them
# (the lightest load breaks a tie), and the rest is cut by weight around them.
HEAVY_JOBS='^(difftest|hardened|fuzz|release-library-regress|tap-shell-core/scaling_slope)'
# shard_loads N POOL: SHARD_OF[job] (0-based shard of the pool) and LOAD[k] for the jobs of the pool, heaviest first, ties in queue order
declare -A SHARD_OF
shard_loads() {
	local pool=$1 line id k i
	SHARD_OF=() LOAD=()
	for (( k = 0; k < pool; k++ )); do LOAD[k]=0; done
	for id in "${J_ID[@]}"; do [[ -v J_AFTER[$id] ]] && { weight_of "$id"; LOAD[pool - 1]=$(( LOAD[pool - 1] + WEIGHT * AFTER_LANES )); }; done
	for id in "${J_ID[@]}"; do [[ -v J_AFTER[$id] ]] || ! pinned "$id" || { weight_of "$id"; SHARD_OF[$id]=0 LOAD[0]=$(( LOAD[0] + WEIGHT )); }; done
	local -a NHEAVY=()
	for (( k = 0; k < pool; k++ )); do NHEAVY[k]=0; done
	while read -r line; do
		id=${line#* }; k=0
		for (( i = 1; i < pool; i++ )); do (( NHEAVY[i] < NHEAVY[k] || (NHEAVY[i] == NHEAVY[k] && LOAD[i] < LOAD[k]) )) && k=$i; done
		SHARD_OF[$id]=$k LOAD[k]=$(( LOAD[k] + ${line% *} )) NHEAVY[k]=$(( NHEAVY[k] + 1 ))
	done < <(for id in "${J_ID[@]}"; do [[ -v J_AFTER[$id] ]] || pinned "$id" || [[ ! $id =~ $HEAVY_JOBS ]] || { weight_of "$id"; echo "$WEIGHT $id"; }; done | sort -s -k1,1nr)
	while read -r line; do
		id=${line#* }; k=0
		for (( i = 1; i < pool; i++ )); do (( LOAD[i] < LOAD[k] )) && k=$i; done
		SHARD_OF[$id]=$k LOAD[k]=$(( LOAD[k] + ${line% *} ))
	done < <(for id in "${J_ID[@]}"; do [[ -v J_AFTER[$id] ]] || pinned "$id" || [[ $id =~ $HEAVY_JOBS ]] || { weight_of "$id"; echo "$WEIGHT $id"; }; done | sort -s -k1,1nr)
}
# heaviest_first: J_ID by weight, ties in queue order
heaviest_first() {
	local id
	mapfile -t J_ID < <(for id in "${J_ID[@]}"; do weight_of "$id"; echo "$WEIGHT $id"; done | sort -s -k1,1nr | cut -d' ' -f2)
}
heaviest_first
# --plan with SIFT_CI_SHARD=all/N: the plans of the N shards in one run, each line prefixed with the number of its shard (--selftest asks for them all)
if [[ -n $PLAN && ${SIFT_CI_SHARD:-} =~ ^all/([0-9]+)$ ]]; then
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
	(( NSHARDS > 1 )) && shard_loads "$NSHARDS"
	for id in "${J_ID[@]}"; do
		if [[ -v J_AFTER[$id] ]]; then (( NSHARDS > 1 && SHARD == NSHARDS )) || (( NSHARDS == 1 )) || continue
		else (( NSHARDS == 1 || SHARD_OF[$id] == SHARD - 1 )) || continue; fi
		KEEP_IDS+=("$id")
		[[ " ${SUITES[*]-} " == *" ${J_SUITE[$id]} "* ]] || SUITES+=("${J_SUITE[$id]}")
	done
	J_ID=("${KEEP_IDS[@]}")
fi
(( LANES <= ${#J_ID[@]} )) || LANES=$(( ${#J_ID[@]} > 0 ? ${#J_ID[@]} : 1 ))   # a shard with no job still builds
# --toolchain: yes when a job of this shard compiles (the workflow installs the compiler and the headers only then), else no; --history: yes when a job of this shard reads a release tag
# (the workflow fetches the history of the tags only then), else no
if [[ $PLAN == toolchain || $PLAN == history ]]; then
	for id in "${J_ID[@]}"; do needs_${PLAN/history/git_history} "$id" && { echo yes; exit 0; }; done
	echo no
	exit 0
fi
if [[ -n $PLAN ]]; then
	for id in "${J_ID[@]}"; do where=pool; [[ -v J_AFTER[$id] ]] && where=after; echo "$id ${J_SUITE[$id]} $where ${J_CMD[$id]}"; done   # no $(...) here: a fork for each of 400 jobs
	exit 0
fi
# a run that selects no job would build, run the harness and exit 0 with no test run (a shard may be empty and still builds)
(( ${#J_ID[@]} || ${NSHARDS:-1} > 1 )) || { echo "ci: no test job selected (SIFT_CI_JOBS='${SIFT_CI_JOBS:-}' SIFT_CI_AFFECTED='${SIFT_CI_AFFECTED:-}')" >&2; exit 1; }
