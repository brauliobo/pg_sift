#!/usr/bin/env bash
# The claim of scripts/ci.sh (scripts/ci/claim.sh: claim, claim_locked) survives what other lanes do to its files: a lane removes its entry of running/ without the lock, and a lane that dies
# holding claim.lock must not hang the others. Seconds, no server.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -uo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
bad=0
fail() { echo "FAIL: $*"; bad=1; }
export TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT   # the lanes a watcher kills leave their scratch directory behind: 636 of them were in /tmp
# one claim in a fresh $BASE, in a subshell that a watcher kills after $2 seconds (default 10; no timeout(1) on macOS): $1 prepares it, the output is "rc CLAIMED lock=state"
claim_once() {
	local out pid watcher
	out=$(mktemp "$TMP/out.XXXXXX")
	"$BASH" -c '
		set -uo pipefail
		eval "$(sed -n "/^claim() {/,\$p" "$1/scripts/ci/claim.sh")"
		BASE=$(mktemp -d "$TMP/base.XXXXXX"); mkdir -p "$BASE/claims" "$BASE/running"; LANE=lane1
		J_ID=(a b); declare -A EST_MB=() J_AFTER=(); BUDGET_MB=100 JOB_MB=70
		eval "$2"
		claim && rc=0 || rc=$?
		echo "$rc ${CLAIMED:-} lock=$([[ -d $BASE/claim.lock ]] && echo held || echo free)"
		rm -rf "$BASE"' _ "$REPO" "$1" >"$out" 2>&1 &
	pid=$!
	( exec >/dev/null 2>&1; sleep "${2:-10}"; kill "$pid" ) &
	watcher=$!
	wait "$pid" 2>/dev/null
	kill "$watcher" 2>/dev/null; wait "$watcher" 2>/dev/null
	cat "$out"; rm -f "$out"
}
# the watchdog ends with the run that started it, also when that run was killed without a trap (a loop that outlived its parent once kept a tree of processes alive)
out=$("$BASH" -c '
	eval "$(sed -n "/^watchdog() {/,/^}/p" "$1/scripts/ci/scratch.sh")"
	BASE=$(mktemp -d "$TMP/base.XXXXXX") LOG=$BASE MAX_MB=999999999
	( exit 0 ) & MAIN=$!; wait "$MAIN"
	watchdog & w=$!
	for _ in {1..50}; do kill -0 "$w" 2>/dev/null || { echo ended; break; }; sleep 0.2; done
	kill "$w" 2>/dev/null; wait "$w" 2>/dev/null; rm -rf "$BASE"' _ "$REPO" 2>&1)
[[ $out == ended ]] || fail "the watchdog of a dead run keeps running: $out"
# a script that sources ci.sh is the $0 of its calls: every `"$0" --plan` started that script again, and so on without end (a profiling harness did, with 41,665 processes
# below one `bash /tmp/tmp.X quick`; bounded to 600 tasks the old ci.sh was still running after 12 s). ci.sh refuses to run outside a checkout, and stops at depth 8 inside one
if command -v timeout >/dev/null; then
	printf '#!/usr/bin/env bash\n. "%s/scripts/ci.sh" affected --list all\n' "$REPO" >"$TMP/harness.sh"
	out=$(timeout -k 2 30 bash "$TMP/harness.sh" 2>&1); rc=$?
	(( rc != 0 && rc != 124 )) && [[ $out == *'from the scripts/ of a checkout'* ]] || fail "a script that sources ci.sh from outside a checkout is not stopped at once (exit $rc): ${out:0:200}"
fi
# a running/ entry that cannot be read (removed between the glob and the read, here a dangling link) or is empty counts for nothing
out=$(claim_once 'ln -s "$BASE/gone" "$BASE/running/x"; : >"$BASE/running/y"')
[[ $out == '0 a lock=free' ]] || fail "a vanished entry of running/ stops the claim: $out"
out=$(claim_once 'echo 20 >"$BASE/running/x"')
[[ $out == '0 a lock=free' ]] || fail "a job that fits next to a running one is refused: $out"
out=$(claim_once 'echo 70 >"$BASE/running/x"')
[[ $out == '2  lock=free' ]] || fail "the claim does not count a running job: $out"
# the lock of a lane that died: its pid is gone, the claim removes the lock and goes on
out=$(claim_once 'mkdir "$BASE/claim.lock"; ( exit 0 ) & wait $!; echo $! >"$BASE/claim.lock/pid"')
[[ $out == *'removes the claim lock'*'0 a lock=free' ]] || fail "the lock of a dead lane is not removed: $out"
# the lock of a live lane is respected: the claim waits (here until the time limit)
out=$(claim_once 'mkdir "$BASE/claim.lock"; echo $$ >"$BASE/claim.lock/pid"; sleep 3 & echo $! >"$BASE/claim.lock/pid"' 2)
[[ -z $out ]] || fail "the lock of a live lane was taken: $out"
exit $bad
