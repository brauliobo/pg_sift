#!/usr/bin/env bash
# The one measurement of a backend's user-space work by perf stat, for bench/steps/ab.sh, bench/cpu-query.sh and steps/analytics.py.
#
#   perf_counts PID [-- CMD...]   (sourced; or executed: steps/perf.sh PID [-- CMD...])
#
# Counts cycles:u, instructions:u and task-clock of the process PID and its children while CMD runs (its output is discarded), or, without
# CMD, until PID exits or SIGINT / SIGTERM reaches the shell that runs it (steps/analytics.py ends the count of one statement that way).
# Prints one line, "CYCLES INSTRUCTIONS TASK_CLOCK_MS" (NA for a counter that perf could not count). Needs perf_event_paranoid <= 2 for
# processes of the user. The function takes over the INT and TERM traps of its shell: call it in a subshell ($(...), < <(...)).
perf_counts() {
	local pid=$1 out p rc sig=
	shift
	out=$(mktemp)
	perf stat -x, -e cycles:u,instructions:u,task-clock -p "$pid" -o "$out" "$@" >/dev/null &
	p=$!
	trap 'kill -INT "$p"; sig=1' INT TERM
	wait "$p" && rc=0 || rc=$?
	[[ -z $sig ]] || { wait "$p" && rc=0 || rc=$?; }   # the signal ended the first wait, not perf
	trap - INT TERM
	awk -F, -v ok='^[0-9.]+$' '/cycles/ { c = $1 } /instructions/ { i = $1 } /task-clock/ { t = $1 }
		function f(x) { return x ~ ok ? x : "NA" }
		END { print f(c), f(i), f(t) }' "$out"
	rm -f "$out"
	return "$rc"
}
[[ ${BASH_SOURCE[0]} != "$0" ]] || perf_counts "$@"
