#!/usr/bin/env bash
# Footprint sampler of scripts/ci.sh (SIFT_CI_STATS=1): every second, the disk blocks of the scratch directory by component and
# by lane, and the memory (PSS) of the postgres processes that run in it. Prints the peak of each, and the sample of the peak
# total, when it gets TERM.
#
#   scripts/ci_stats.sh SCRATCH_DIR
#
# The jobs: the peak of the lane directory while a job ran in it (the lane runs one job at a time, so it is the job's, with the
# cluster of the lane under it).
#
# Components: pg_wal, base (table and index files), temp (pgsql_tmp), data (the rest of a data directory), logs, regress (output of
# pg_regress), work (fuzz, difftest, soak, isolation, sanitizer reports), install (the staged build, the initdb templates), tap
# (what else is in a job directory), other.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -uo pipefail
BASE=$1
declare -A peak lane_peak job_peak lane_size
total_peak=0 rss_peak=0 at_peak=

# shellcheck disable=SC2016  # awk program
classify='{
	b = $1 * 512; p = $2
	split(p, d, "/"); lane = d[1]
	if (p ~ /(^|\/)pg_wal\//) c = "pg_wal"
	else if (p ~ /pgsql_tmp/) c = "temp"
	else if (p ~ /(^|\/)base\//) c = "base"
	else if (p ~ /(^|\/)regress\//) c = "regress"
	else if (p ~ /(^|\/)(data|pgdata)\// ) c = "data"
	else if (p ~ /\.log$|\/log\//) c = "logs"
	else if (p ~ /(^|\/)(fuzz|difftest|soak|isolation|sanitizer)\//) c = "work"
	else if (p ~ /^(root|initdb-)/) c = "install"
	else if (p ~ /^logs\//) c = "logs"
	else if (p ~ /\/tap/) c = "tap"
	else c = "other"
	comp[c] += b; ln[lane] += b; tot += b
}
END { for (c in comp) print "c", c, comp[c]; for (l in ln) print "l", l, ln[l]; print "t", "total", tot }'

pss_mb() { # postgres processes of this scratch directory
	local pid kb=0 k
	for pid in $(pgrep -u "$UID" -x 'postgres|postmaster' 2>/dev/null); do
		[[ $(readlink "/proc/$pid/cwd" 2>/dev/null) == "$BASE"/* ]] || continue
		k=$(awk '/^Pss:/ { print $2 }' "/proc/$pid/smaps_rollup" 2>/dev/null) && kb=$((kb + ${k:-0}))
	done
	echo $((kb / 1024))
}

running_jobs() { # claimed jobs without a result, in the lane that wrote its name in the claim
	local c id lane
	for c in "$BASE"/claims/*/lane; do
		[[ -f $c ]] || continue
		id=$(basename "$(dirname "$c")") lane=$(<"$c")
		grep -q "^$id " "$BASE/logs/results" 2>/dev/null && continue
		(( ${lane_size[$lane]:-0} > ${job_peak[$id]:-0} )) && job_peak[$id]=${lane_size[$lane]}
	done
}

report() {
	local k
	echo "scratch peak by component (MB, each at its own peak; the sum of the peaks is higher than the peak total):"
	for k in pg_wal base data temp logs regress work install tap other; do printf '  %-8s %6d\n' "$k" $(( ${peak[c $k]:-0} / 1048576 )); done
	echo "  peak total $((total_peak / 1048576)) MB at: ${at_peak}"
	echo "scratch peak by lane (MB):"
	for k in "${!lane_peak[@]}"; do printf '  %-14s %6d\n' "$k" $(( ${lane_peak[$k]} / 1048576 )); done | sort
	echo "scratch peak by job (MB, the lane directory while it ran):"
	for k in "${!job_peak[@]}"; do printf '  %-40s %6d\n' "$k" $(( ${job_peak[$k]} / 1048576 )); done | sort -k2 -n -r | head -"${JOBS_SHOWN:-12}"
	echo "postgres processes, peak PSS: $rss_peak MB"
}
trap 'report; exit 0' TERM INT

while :; do
	sample=$(find "$BASE" -type f -printf '%b %P\n' 2>/dev/null | awk "$classify")
	line='' total=0
	while read -r kind name bytes; do
		case $kind in
			c) (( bytes > ${peak[c $name]:-0} )) && peak[c $name]=$bytes; line+="$name=$((bytes / 1048576)) " ;;
			l) lane_size[$name]=$bytes; (( bytes > ${lane_peak[$name]:-0} )) && lane_peak[$name]=$bytes ;;
			t) total=$bytes ;;
		esac
	done <<<"$sample"
	(( total > total_peak )) && { total_peak=$total; at_peak=$line; }
	running_jobs
	rss=$(pss_mb)
	(( rss > rss_peak )) && rss_peak=$rss
	sleep 1 &
	wait $!
done
