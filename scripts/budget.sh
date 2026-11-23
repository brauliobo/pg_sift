#!/usr/bin/env bash
# Bounded runs: every entry point of the tests and benchmarks has a row in scripts/budgets.tsv (wall seconds, peak tmpfs MB) and runs under it.
#
#   scripts/budget.sh run NAME CMD...   CMD under the budget of NAME; a run over its wall or tmpfs budget is stopped and reported as a BUDGET FAILURE
#                                       (exit 124 wall, 125 tmpfs) with the slowest steps of $BUDGET_STEPS. Without SIFT_TMP the run gets a scratch
#                                       directory of its own on tmpfs, removed on exit; on failure only its logs and repro files (at most 20 MB) are kept in
#                                       $BUDGET_KEEP/NAME.<time> (default /dev/shm/sift-budget-fail, or under $TMPDIR where there is no /dev/shm). A scratch directory the caller made is the caller's to remove.
#   scripts/budget.sh tap FILE          for prove --exec: the TAP file under tap:<name>, else tap:default
#   scripts/budget.sh step NAME CMD...  CMD, its wall seconds appended to $BUDGET_STEPS (the slowest steps of a budget failure)
#   scripts/budget.sh limits NAME       "SECONDS MB" of NAME
#   scripts/budget.sh list              the table
# BUDGET_RESULTS=FILE appends "label rc wall_s limit_s peak_mb limit_mb" per run (the label is BUDGET_LABEL, else the name). The budget is the owner's: raise a row only with the measurement that justifies it.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
command -v timeout >/dev/null || { echo 'needs timeout (macOS: brew install coreutils, then put gtimeout on PATH as timeout)' >&2; exit 1; }
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
TABLE=${BUDGET_TABLE:-$HERE/budgets.tsv}
source "$HERE/ci/procs.sh"
OWN=
cleanup() { [[ -z $OWN ]] || { stop_postmasters "$OWN"; rm -rf "$OWN"; }; }
trap cleanup EXIT

limits() {
	awk -F'\t' -v n="$1" '$1 !~ /^#/ && $1 == n { print $2, $3; found = 1; exit } END { exit !found }' "$TABLE" \
		|| { echo "budget: no row for '$1' in $TABLE: every entry point needs one (scripts/check-budgets.sh)" >&2; return 2; }
}

peak_mb() { du -sm "$1" 2>/dev/null | cut -f1; }

keep_failure() { # SCRATCH NAME: logs and small repro files only
	local dst=$BUDGET_KEEP/$2.$(date +%H%M%S) f
	mkdir -p "$dst"
	( cd "$1" && find . -type f \( -name '*.log' -o -name '*.diffs' -o -name 'FAIL-*' -o -name summary.txt -o -name '*.out' \) -size -2M ) | while read -r f; do
		(( $(du -sk "$dst" | cut -f1) < 20480 )) && (cd "$1" && mkdir -p "$dst/${f%/*}" && cp "$f" "$dst/$f")
	done
	echo "budget: failure files in $dst"
}

run() {
	local name=$1 secs mb scratch pid rc= start=$SECONDS peak=0 now why= who st tick
	shift
	read -r secs mb < <(limits "$name") || return 2
	secs=${BUDGET_SECS:-$secs}
	if [[ -n ${BUDGET_ACTIVE:-} ]]; then # inside a run: the wall budget of this entry point only; the scratch and the results are the outer run's
		timeout -k 30 "$secs" "$@"; rc=$?
		(( rc != 124 )) || echo "BUDGET FAILURE: $name: wall budget of $secs s exceeded" >&2
		return "$rc"
	fi
	export BUDGET_ACTIVE=$name
	local shm=/dev/shm
	[[ -d $shm && -w $shm ]] || shm=${TMPDIR:-/tmp}
	BUDGET_KEEP=${BUDGET_KEEP:-$shm/sift-budget-fail}
	if [[ -z ${SIFT_TMP:-} ]]; then
		scratch=$(mktemp -d "$shm/sift-budget-${name//[^A-Za-z0-9_.-]/_}.XXXXXX") && OWN=$scratch
		export SIFT_TMP=$scratch
	fi
	scratch=${BUDGET_SCRATCH:-$SIFT_TMP}
	mkdir -p "$scratch"
	export BUDGET_STEPS=${BUDGET_STEPS:-$SIFT_TMP/steps.tsv}
	timeout -k 30 "$secs" "$@" & pid=$!
	trap 'kill -TERM "$pid" 2>/dev/null' INT TERM
	while [[ -z $rc ]]; do
		now=$(peak_mb "$scratch"); (( now > peak )) && peak=$now
		(( now > mb )) && { why=tmpfs; kill -TERM "$pid" 2>/dev/null; wait "$pid"; rc=$?; break; }
		sleep 5 & tick=$!
		who=; wait -n -p who "$pid" "$tick"; st=$?
		[[ $who == "$pid" ]] && rc=$st
		kill "$tick" 2>/dev/null
	done
	trap - INT TERM
	now=$(peak_mb "$scratch"); (( now > peak )) && peak=$now
	[[ $why == tmpfs ]] && rc=125
	if (( rc == 124 || rc == 137 )) && (( SECONDS - start >= secs )); then why=wall; rc=124; fi
	if [[ -n $why ]]; then
		echo "BUDGET FAILURE: $name: $why budget exceeded (wall $((SECONDS - start)) s of $secs s, peak $peak MB of $mb MB)" >&2
		echo "slowest steps:" >&2
		{ sort -rn "$BUDGET_STEPS" 2>/dev/null | head -5 | sed 's/^/  /'; } >&2
	fi
	(( rc == 0 )) || stop_postmasters "$scratch"
	(( rc == 0 )) || keep_failure "$scratch" "${name//[^A-Za-z0-9_.-]/_}"
	[[ -z ${BUDGET_RESULTS:-} ]] || printf '%s\t%s\t%s\t%s\t%s\t%s\n' "${BUDGET_LABEL:-$name}" "$rc" "$((SECONDS - start))" "$secs" "$peak" "$mb" >>"$BUDGET_RESULTS"
	echo "budget $name: rc=$rc wall=$((SECONDS - start))/${secs} s, tmpfs peak=${peak}/${mb} MB" >&2
	return "$rc"
}

tap() {
	local name
	name=$(basename "$1"); name=${name%.*}
	limits "tap:$name" >/dev/null 2>&1 || name=default
	BUDGET_SCRATCH=${SIFT_TMP:?}/tap "$0" run "tap:$name" bash "$@"
}

step() {
	local name=$1 s=$SECONDS rc
	shift
	"$@"; rc=$?
	[[ -z ${BUDGET_STEPS:-} ]] || printf '%s\t%s\n' "$((SECONDS - s))" "$name" >>"$BUDGET_STEPS"
	return $rc
}

case ${1:-} in
	run) shift; run "$@" ;;
	tap) shift; tap "$@" ;;
	step) shift; step "$@" ;;
	limits) limits "${2:?}" ;;
	list) grep -v '^#' "$TABLE" ;;
	*) sed -n '2,13p' "$0"; exit 2 ;;
esac
