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
