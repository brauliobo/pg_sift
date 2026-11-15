#!/usr/bin/env bash
# The bounded stability run: the fuzzers, oracle seeds and crash tests of scripts/stability.tsv in parallel lanes, each step under its budget (scripts/budget.sh).
#
#   scripts/stability.sh [--round N] [--lanes N] [--only REGEX] [--list] [--no-install]
#
# A fuzzer or oracle never runs every seed: the pools of seeds are in the @pool lines of scripts/stability.tsv and each round takes the next window of every pool, so
# the rounds of consecutive runs cover the whole pool and a run costs one window (--round defaults to the number of commits of HEAD: the seeds of a run are known from its
# commit, and a failure is repeated with --round N). A seed of a failure is printed with the step. The steps queue longest first (their estimate, est_s) onto the lanes;
# each lane has its own SIFT_TMP and ports, all share one install and one initdb template. The whole run has the budget of gate:stability, each step that of its row.
# Scratch lives under $STABILITY_TMP (default /dev/shm/sift-stability-<pid>), removed on exit; a failed step keeps its logs and repro files only (scripts/budget.sh).
# Results: one line per step in $STABILITY_TMP/results.tsv, printed as a table at the end; exit 1 when a step failed or was over its budget.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
S=$REPO/scripts
PLAN=${STABILITY_PLAN:-$S/stability.tsv}
ROUND=$(git -C "$REPO" rev-list --count HEAD 2>/dev/null || echo 0) LANES=4 ONLY=. LIST= INSTALL=1
while (( $# )); do case $1 in
	--round) ROUND=${2:?}; shift ;; --lanes) LANES=${2:?}; shift ;; --only) ONLY=${2:?}; shift ;; --list) LIST=1 ;; --no-install) INSTALL= ;;
	*) sed -n '2,13p' "$0"; exit 2 ;; esac; shift
done
(( LANES >= 1 && LANES <= 8 )) || { echo "stability: --lanes 1-8 (the ports of a lane are SIFT_PORT + 10 per lane in 56100-56299)" >&2; exit 2; }

declare -A POOL SLOTS LANE_OF
while IFS=$'\t' read -r tag key pool; do [[ $tag == @pool ]] && POOL[$key]=$pool; done < "$PLAN"
while read -r key slot; do (( slot + 1 > ${SLOTS[$key]:-0} )) && SLOTS[$key]=$((slot + 1)); done \
	< <(grep -v '^[#@]' "$PLAN" | grep -oE '\{seeds:[a-z0-9-]+:[0-9]+:[0-9]+\}' | tr -d '{}' | awk -F: '{ print $2, $4 }')

# window KEY COUNT SLOT: COUNT seeds of the pool of KEY, the chunk (ROUND * slots of KEY + SLOT) of the pool taken in order and wrapping, as a comma list
window() {
	local all=() p out=() i start
	for p in $(tr ',' ' ' <<<"${POOL[$1]:?no @pool $1 in $PLAN}"); do
		if [[ $p == *-* ]]; then all+=($(seq "${p%-*}" "${p#*-}")); else all+=("$p"); fi
	done
	start=$(( (ROUND * ${SLOTS[$1]} + $3) * $2 % ${#all[@]} ))
	for (( i = 0; i < $2; i++ )); do out+=("${all[(start + i) % ${#all[@]}]}"); done
	(IFS=,; echo "${out[*]}")
}
expand() { # the {seeds:KEY:COUNT:SLOT} of a command
	local cmd=$1 m key count slot
	while [[ $cmd =~ \{seeds:([a-z0-9-]+):([0-9]+):([0-9]+)\} ]]; do
		m=${BASH_REMATCH[0]} key=${BASH_REMATCH[1]} count=${BASH_REMATCH[2]} slot=${BASH_REMATCH[3]}
		cmd=${cmd/"$m"/$(window "$key" "$count" "$slot")}
	done
	echo "$cmd"
}

steps=() # est TAB step TAB budget TAB cluster TAB command, longest first
while IFS=$'\t' read -r step budget cluster est cmd; do
	[[ $step =~ $ONLY ]] && steps+=("$est"$'\t'"$step"$'\t'"$budget"$'\t'"$cluster"$'\t'"$(expand "$cmd")")
done < <(grep -v '^[#@]' "$PLAN" | grep -v '^$')
mapfile -t steps < <(printf '%s\n' "${steps[@]}" | sort -t$'\t' -k1,1nr)
if [[ -n $LIST ]]; then
	echo "round $ROUND, $LANES lanes"; printf '%s\n' "${steps[@]}" | awk -F'\t' '{ printf "%-22s %-28s est %4ss  %s\n", $2, $3, $1, $5 }'
	exit 0
fi

BASE=${STABILITY_TMP:-/dev/shm/sift-stability-$$}
mkdir -p "$BASE" && : > "$BASE/results.tsv"
source "$S/ci/procs.sh"
cleanup() { stop_postmasters "$BASE"; rm -rf "$BASE"; }
trap cleanup EXIT
trap 'kill -TERM "${!LANE_OF[@]}" 2>/dev/null; wait; exit 130' INT TERM
export SIFT_ROOT=$BASE/root SIFT_INITDB_TEMPLATE=$BASE/initdb BUDGET_RESULTS=$BASE/results.tsv BUDGET_STEPS=$BASE/steps.tsv
if [[ -n $INSTALL ]]; then
	SIFT_TMP=$BASE/install "$S/pgtest.sh" install > "$BASE/install.log" 2>&1 || { tail -n 20 "$BASE/install.log"; echo "stability: install failed"; exit 1; }
fi
"$S/pgtest.sh" initdb-template "$SIFT_INITDB_TEMPLATE" || exit 1

lane_step() { # LANE EST STEP BUDGET CLUSTER CMD
	local lane=$1 step=$3 budget=$4 cluster=$5 cmd=$6
	local dir=$BASE/$step
	mkdir -p "$dir"
	[[ $cluster == 1 ]] && cmd="$S/pgtest.sh start >/dev/null && eval \"\$($S/pgtest.sh env)\" && { $cmd; }; rc=\$?; $S/pgtest.sh stop >/dev/null 2>&1; exit \$rc"
	( cd "$REPO" && SIFT_TMP=$dir SIFT_PORT=$((56100 + 10 * lane)) BUDGET_SCRATCH=$dir BUDGET_LABEL=$step "$S/budget.sh" run "$budget" bash -c "$cmd" ) > "$dir.log" 2>&1
	local rc=$?
	(( rc == 0 )) || { echo "stability: $step FAILED rc=$rc (round $ROUND)"; tail -n 15 "$dir.log"; }
	rm -rf "$dir" "$dir.log"
	return "$rc"
}

start=$SECONDS failed=0 running=0 next=0 free=()
for (( l = 0; l < LANES; l++ )); do free+=("$l"); done
while (( next < ${#steps[@]} || running > 0 )); do
	if (( next < ${#steps[@]} && ${#free[@]} > 0 )); then
		IFS=$'\t' read -r est step budget cluster cmd <<<"${steps[next]}"
		lane=${free[0]}; free=("${free[@]:1}")
		lane_step "$lane" "$est" "$step" "$budget" "$cluster" "$cmd" & LANE_OF[$!]=$lane
		next=$((next + 1)) running=$((running + 1))
		continue
	fi
	wait -n -p done_pid; rc=$?
	(( rc == 0 )) || failed=1
	free+=("${LANE_OF[$done_pid]}"); running=$((running - 1))
done

total=$((SECONDS - start))
printf '\nround %s, %s lanes, wall %s s of the budget %s s\n%-34s %3s %7s %7s %8s %8s\n' "$ROUND" "$LANES" "$total" "$(awk -F'\t' '$1 == "gate:stability" { print $2 }' "$S/budgets.tsv")" step rc wall_s limit_s peak_mb limit_mb
sort -t$'\t' -k3,3nr "$BASE/results.tsv" | awk -F'\t' '{ printf "%-34s %3s %7s %7s %8s %8s\n", $1, $2, $3, $4, $5, $6 }'
limit=$(awk -F'\t' '$1 == "gate:stability" { print $2 }' "$S/budgets.tsv")
(( total <= limit )) || { echo "BUDGET FAILURE: stability: the run took $total s of $limit s; slowest steps above" >&2; failed=1; }
exit "$failed"
