#!/usr/bin/env bash
# Shared dependency-map owner: Bash ERE, exact known groups and captures 1..3.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
declare -A GROUP
ROWS=() FAST_ROWS=()
areas_load() {
	local k v
	GROUP=() ROWS=() FAST_ROWS=()
	while IFS=$'\t' read -r k v; do
		[[ -z $k || $k == '#'* ]] && continue
		if [[ $k == =* ]]; then GROUP[${k#=}]=$v
		elif [[ $v == fast:* ]]; then FAST_ROWS+=("$k"$'\t'"${v#fast:}")   # only the fast tier of a pull request reads these rows
		else ROWS+=("$k"$'\t'"$v"); fi
	done <"$1"
}
areas_match() {
	local rc
	[[ $1 =~ $2 ]]; rc=$?
	if (( rc == 2 )); then echo "areas: invalid ERE: $2" >&2; exit 2; fi
	return "$rc"
}
# Expand using the snapshot of the path match, before any other ERE is tested.
# In owner mode, explicit owners are routed directly, leaving only residual jobs.
areas_expand() {
	local g n replacement
	AREA_JOBS=$1
	for g in "${!GROUP[@]}"; do
		replacement="(${GROUP[$g]})"
		if [[ ${AREA_OWNER_MODE:-} == 1 && " ${AREA_OWNERS[*]} " == *" $g "* ]]; then replacement='(^$)'; fi
		AREA_JOBS=${AREA_JOBS//\{$g\}/$replacement}
	done
	for n in 1 2 3; do AREA_JOBS=${AREA_JOBS//\\$n/${AREA_CAPTURES[$n]-}}; done
	# Compile even when there are no candidate jobs.
	areas_match '' "$AREA_JOBS" || :
}
areas_regex() {
	local f row hit out=() AREA_JOBS AREA_OWNER_MODE=0
	local -A seen=()   # a diff of hundreds of files would repeat each row hundreds of times: the workflow output limit is 1 MB
	local -a AREA_CAPTURES
	for f in "$@"; do
		hit=
		for row in "${ROWS[@]}"; do
			areas_match "$f" "${row%%$'\t'*}" || continue
			AREA_CAPTURES=("${BASH_REMATCH[@]}")
			hit=1
			areas_expand "${row#*$'\t'}"
			[[ $AREA_JOBS == - || -n ${seen[$AREA_JOBS]:-} ]] || { seen[$AREA_JOBS]=1; out+=("$AREA_JOBS"); }
		done
		[[ -n $hit ]] || { echo "areas: $f is in no row of scripts/areas.tsv" >&2; out+=(.); }
	done
	local IFS='|'
	echo "^\$${out[*]:+|${out[*]}}"
}
# areas_unmatched FILE...: the files that no row matches, found with the paths of all rows as one ERE (areas_regex runs every row on every file, which a map of
# every tracked file makes 400,000 matches)
areas_unmatched() {
	local f rx
	rx=$(printf '(%s)|' "${ROWS[@]%%$'\t'*}"); rx=${rx%|}
	for f in "$@"; do [[ $f =~ $rx ]] || echo "$f"; done
}
# areas_invalid: a line for each row whose path or jobs (with the groups filled in and no captures) is not a valid ERE
areas_invalid() {
	local row AREA_JOBS out
	local -a AREA_CAPTURES=("" "" "" "")
	for row in "${ROWS[@]}" "${FAST_ROWS[@]}"; do
		out=$( ( areas_match '' "${row%%$'\t'*}" || :; areas_expand "${row#*$'\t'}" ) 2>&1 ) || echo "$out"
	done
}
# areas_fast_regex FILE...: areas_regex, except that a file a `fast:` row matches runs only the jobs of that row (the tests of its own area, not of the
# stack above it); the other files run what areas_regex gives them
areas_fast_regex() {
	local f row hit out=() AREA_JOBS AREA_OWNER_MODE=0 alt
	local -A seen=()   # the same: a branch of hundreds of files made 134 KB, past the 128 KB limit of one argument of the jq call that builds the matrix
	local -a AREA_CAPTURES
	for f in "$@"; do
		hit=
		for row in "${FAST_ROWS[@]}"; do
			areas_match "$f" "${row%%$'\t'*}" || continue
			AREA_CAPTURES=("${BASH_REMATCH[@]}")
			hit=1
			areas_expand "${row#*$'\t'}"
			[[ -n ${seen[$AREA_JOBS]:-} ]] || { seen[$AREA_JOBS]=1; out+=("$AREA_JOBS"); }
			break
		done
		[[ -n $hit ]] && continue
		alt=$(areas_regex "$f")
		[[ -n ${seen[$alt]:-} ]] || { seen[$alt]=1; out+=("$alt"); }
	done
	local IFS='|'
	echo "^\$${out[*]:+|${out[*]}}"
}
# One batch: jobs on NUL-delimited stdin, paths in argv, owner keys before --.
areas_owners() {
	local -a AREA_OWNERS=() AREA_CAPTURES JOBS=()
	local f row value g job hit live AREA_JOBS AREA_OWNER_MODE=1
	while [[ $1 != -- ]]; do AREA_OWNERS+=("$1"); shift; done
	shift
	mapfile -d '' -t JOBS
	for f in "$@"; do
		hit='' live=''
		for row in "${ROWS[@]}"; do
			areas_match "$f" "${row%%$'\t'*}" || continue
			AREA_CAPTURES=("${BASH_REMATCH[@]}")
			hit=1 value=${row#*$'\t'}
			# `-`, {perf} (the perf harness and CI machinery) and {ci} (what only CI jobs can break) never change what a paired run times.
			[[ $value == - || $value == *'{perf}'* || $value == *'{ci}'* ]] && continue
			live=1
			if [[ $value == . ]]; then echo '*'; continue; fi
			for g in "${AREA_OWNERS[@]}"; do [[ $value == *"{$g}"* ]] && echo "$g"; done
			areas_expand "$value"
			for job in "${JOBS[@]}"; do
				areas_match "$job" "$AREA_JOBS" || continue
				for g in "${AREA_OWNERS[@]}"; do areas_match "$job" "${GROUP[$g]}" && echo "$g"; done
			done
		done
		[[ -z $hit ]] && echo '*'
		[[ -n $hit && -z $live ]] && echo '-'
	done
	return 0
}
if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	set -uo pipefail
	areas_load "$2"
	mode=$1; shift 2
	case $mode in
		--owners) areas_owners "$@" ;;
		--regex) areas_regex "$@" ;;
		*) echo 'areas: expected --owners or --regex' >&2; exit 2 ;;
	esac
fi
