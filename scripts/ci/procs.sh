# Process helpers: the descendants of a pid, the postmasters under a directory, and the reaper of the scratch directories of dead runs.
# Used by the selftest, the cleanup and the start of a run. Sourced by scripts/ci.sh; not a script of its own.
# descendants PID: the processes below PID, deepest first
descendants() { local p; for p in $(pgrep -P "$1" 2>/dev/null); do descendants "$p"; echo "$p"; done; }
# stop_postmasters DIR: every postmaster whose data directory is under DIR (a fast shutdown, then QUIT, then KILL, two seconds each); the pid is checked to be a postgres process, never matched by name
stop_postmasters() {
	local f pid i sig
	while read -r f; do
		pid=$(head -n 1 "$f" 2>/dev/null)
		[[ $pid =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null && [[ $(ps -o command= -p "$pid" 2>/dev/null) == *postgres* ]] || continue
		for sig in INT QUIT KILL; do
			kill -"$sig" "$pid" 2>/dev/null
			for (( i = 0; i < 20; i++ )); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
		done
	done < <(find "$1" -name postmaster.pid 2>/dev/null)
}
# reap_stale TMPBASE: the sift-ci-<pid> directories (and their postmasters) of runs that died; a directory a failed run kept for its logs (the file `kept`) stays a day
reap_stale() {
	local d pid
	for d in "$1"/sift-ci-*; do
		pid=${d##*/sift-ci-}
		[[ -d $d && $pid =~ ^[0-9]+$ ]] && ! kill -0 "$pid" 2>/dev/null || continue
		[[ ! -e $d/kept || -n $(find "$d/kept" -mmin +1440 2>/dev/null) ]] || continue
		stop_postmasters "$d"
		rm -rf "$d"
	done
}
# proc_id PID: "PID START", START being the start time of the process (field 22 of /proc/PID/stat; ps elsewhere): a pid that was reused does not have it
proc_id() { echo "$1 $(awk '{ print $22 }' "/proc/$1/stat" 2>/dev/null || ps -o lstart= -p "$1" 2>/dev/null | tr ' ' _)"; }
# stop_run DIR: stops the run that records itself in DIR/run.pid (the pid and start time of its ci.sh, written when the scratch directory is made): TERM to that pid and the processes below it, whose trap stops
# the lanes, their jobs and the postmasters under DIR; KILL to it alone after SIFT_CI_STOP_WAIT seconds (60); the postmasters under DIR are stopped by their pid files. It never
# looks a process up by name or command line: a pid that is not the one recorded (another run's, or reused) is refused, and a run is only named by its directory
stop_run() {
	local dir=$1 rec pid i kids
	[[ -n $dir && -f $dir/run.pid ]] || { echo "ci: stop: no run recorded in '${dir:-}' (the scratch directory of a run has run.pid; name it, e.g. SIFT_CI_TMP)" >&2; return 2; }
	[[ -O $dir/run.pid ]] || { echo "ci: stop: $dir/run.pid is not yours" >&2; return 2; }
	rec=$(<"$dir/run.pid"); pid=${rec%% *}
	[[ $pid =~ ^[0-9]+$ ]] || { echo "ci: stop: $dir/run.pid is malformed" >&2; return 2; }
	if kill -0 "$pid" 2>/dev/null; then
		[[ $(proc_id "$pid") == "$rec" ]] || { echo "ci: stop: pid $pid is not the run of $dir any more (another process has it): left alone" >&2; return 3; }
		kids=$(descendants "$pid")   # by parentage: the run waits in a foreground job (lint, a lane) and takes its TERM only after that ends
		kill -TERM $kids "$pid" 2>/dev/null
		for (( i = 0; i < ${SIFT_CI_STOP_WAIT:-60} * 10; i++ )); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
		if kill -0 "$pid" 2>/dev/null && [[ $(proc_id "$pid") == "$rec" ]]; then kill -KILL "$pid" 2>/dev/null; echo "ci: stop: pid $pid did not end in time, killed" >&2; fi
		echo "ci: stop: run $pid of $dir stopped"
	else
		echo "ci: stop: the run of $dir (pid $pid) is not running"
	fi
	stop_postmasters "$dir"
}
