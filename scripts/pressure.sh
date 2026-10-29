#!/usr/bin/env bash
# The pressure of the box (Linux PSI): prints the `some avg10` and `avg60` of /proc/pressure/cpu and /proc/pressure/io, and exits 1 when either avg10 is
# at or above LIMIT (default 10, percent of the time some task waited). Load average is no gate: it counts tasks, not waiting.
#
#   scripts/pressure.sh           print and test once
#   scripts/pressure.sh --wait    wait (30 s apart, at most SIFT_PRESSURE_WAIT seconds, default 600) until it passes; exit 1 if it never did
#
# No /proc/pressure (macOS, a kernel without PSI): prints nothing and passes.
LIMIT=${SIFT_PRESSURE_LIMIT:-10}
waited=0
while :; do
	busy=0 line=
	for r in cpu io; do
		[[ -r /proc/pressure/$r ]] || continue
		read -r _ a10 a60 _ <<<"$(head -n 1 "/proc/pressure/$r")"
		line+="$r some ${a10/avg10=/avg10 } ${a60/avg60=/avg60 }; "
		awk -v v="${a10#avg10=}" -v l="$LIMIT" 'BEGIN { exit !(v >= l) }' && busy=1
	done
	[[ -z $line ]] && exit 0
	echo "pressure: ${line%; }"
	(( busy )) || exit 0
	[[ ${1:-} == --wait ]] && (( waited < ${SIFT_PRESSURE_WAIT:-600} )) || exit 1
	sleep 30; waited=$((waited + 30))
done
