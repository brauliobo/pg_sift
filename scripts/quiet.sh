#!/usr/bin/env bash
# Is the host quiet enough to time anything? Judged by Linux pressure stall information (/proc/pressure/cpu and io, the
# `some` line: the share of the last 10 s / 60 s in which a task was stalled), not by the load average, which counts
# runnable and uninterruptible tasks and says nothing about how long they waited.
#
# Source it from a shell script, or call it from anywhere (python: subprocess.run([".../quiet.sh", "wait", ...])):
#
#   quiet_wait [--max PCT] [--timeout SEC] [--window avg10|avg60]
#       blocks until both cpu and io are below PCT in the window, two samples 2 s apart in a row. Returns 0 then, 1 when
#       the timeout expires (the pressure is printed to stderr), 2 for a bad argument, 3 when there is no PSI.
#   quiet_batch [same options]
#       for a timed batch: quiet_wait that never fails. Sets QUIET_MARK to "" (quiet) or " NOISY" (timeout, or no PSI
#       to tell), counts the batches in QUIET_BATCHES / QUIET_NOISY. Append "$QUIET_MARK" to what the batch prints.
#   quiet_tag
#       `cpu=X% io=Y%` (some avg10 now) plus QUIET_MARK, for a column of a result table next to the batch's numbers.
#   quiet_report
#       one line: `pressure cpu=X%/Y% io=X%/Y% (some avg10/avg60)`; returns 3 and says so when there is no PSI.
#   quiet_summary
#       one line for the footer: the report and how many of the timed batches were NOISY.
#
#   scripts/quiet.sh wait [options] | report | summary     the same, as a command
#
# Environment: SIFT_QUIET_MAX (percent, default 10), SIFT_QUIET_TIMEOUT (seconds, default 60), SIFT_QUIET_WINDOW (avg10, avg60);
# the options override them. A sourcing script keeps its own shell options: nothing here sets any.
QUIET_NO_PSI=3 QUIET_PROC=${SIFT_QUIET_PROC:-/proc/pressure}   # the second is for testing the host without PSI
QUIET_BATCHES=0 QUIET_NOISY=0 QUIET_MARK=

quiet_have() { # PSI of this kernel: /proc/pressure/{cpu,io}
	[[ -r $QUIET_PROC/cpu && -r $QUIET_PROC/io ]] && return 0
	echo "quiet: cannot measure: /proc/pressure/cpu and io are missing (kernel without CONFIG_PSI, or booted with psi=0)" >&2
	return "$QUIET_NO_PSI"
}
quiet_psi() { # RESOURCE WINDOW: the `some` average of the window, in percent
	awk -v w="$2" '$1 == "some" { for (i = 2; i <= NF; i++) { split($i, kv, "="); if (kv[1] == w) print kv[2] } }' "$QUIET_PROC/$1"
}
quiet_report() {
	quiet_have || return "$QUIET_NO_PSI"
	printf 'pressure cpu=%s%%/%s%% io=%s%%/%s%% (some avg10/avg60)\n' "$(quiet_psi cpu avg10)" "$(quiet_psi cpu avg60)" "$(quiet_psi io avg10)" "$(quiet_psi io avg60)"
}
quiet_wait() {
	local max=${SIFT_QUIET_MAX:-10} timeout=${SIFT_QUIET_TIMEOUT:-60} window=${SIFT_QUIET_WINDOW:-avg10} hold=0 deadline now cpu io
	while (( $# )); do
		case $1 in
			--max) max=${2:?--max PCT}; shift ;;
			--timeout) timeout=${2:?--timeout SEC}; shift ;;
			--window) window=${2:?--window avg10|avg60}; shift ;;
			*) echo "quiet_wait: unknown argument $1 (--max PCT, --timeout SEC, --window avg10|avg60)" >&2; return 2 ;;
		esac
		shift
	done
	[[ $window == avg10 || $window == avg60 ]] || { echo "quiet_wait: the window is avg10 or avg60, not $window" >&2; return 2; }
	quiet_have || return "$QUIET_NO_PSI"
	deadline=$(( $(date +%s) + timeout ))
	while :; do
		cpu=$(quiet_psi cpu "$window") io=$(quiet_psi io "$window")
		if awk -v c="$cpu" -v i="$io" -v m="$max" 'BEGIN { exit !(c < m && i < m) }'; then hold=$(( hold + 1 )); else hold=0; fi
		if (( hold >= 2 )); then return 0; fi
		now=$(date +%s)
		if (( now >= deadline )); then
			echo "quiet_wait: timeout after ${timeout} s: pressure cpu=${cpu}% io=${io}% (some $window), limit ${max}%" >&2
			return 1
		fi
		sleep 2
	done
}
quiet_batch() { # [quiet_wait options]: see above
	local rc=0
	quiet_wait "$@" || rc=$?
	QUIET_BATCHES=$(( QUIET_BATCHES + 1 ))
	if (( rc == 0 )); then QUIET_MARK=; else QUIET_MARK=" NOISY"; QUIET_NOISY=$(( QUIET_NOISY + 1 )); fi
	return 0
}
quiet_tag() {
	quiet_have 2>/dev/null || { printf 'unmeasured NOISY'; return 0; }
	printf 'cpu=%s%% io=%s%%%s' "$(quiet_psi cpu avg10)" "$(quiet_psi io avg10)" "$QUIET_MARK"
}
quiet_summary() {
	local rep rc=0
	rep=$(quiet_report 2>&1) || rc=$?
	if (( rc == QUIET_NO_PSI )); then echo "pressure not measured (no PSI): every timed batch is NOISY"; return "$rc"; fi
	echo "$rep; ${QUIET_NOISY} of ${QUIET_BATCHES} timed batches NOISY (limit ${SIFT_QUIET_MAX:-10}%)"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	case ${1:-} in
		wait) shift; quiet_wait "$@" ;;
		report) quiet_report ;;
		summary) quiet_summary ;;
		*) sed -n '2,27p' "$0"; exit 2 ;;
	esac
fi
