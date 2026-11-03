#!/usr/bin/env bash
# `scripts/ci.sh stop DIR` ends the run recorded in DIR/run.pid by that pid and nothing else: three processes with the same command line (sleep 300), one recorded, one recorded with
# the start time of another process (a reused pid), one not recorded at all; only the first ends. And a run that is not running, a directory with no record. Seconds, no server.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -uo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
bad=0
fail() { echo "FAIL: $*"; bad=1; }
T=$(mktemp -d); pids=()
trap 'kill "${pids[@]}" 2>/dev/null; rm -rf "$T"' EXIT
source "$REPO/scripts/ci/procs.sh"
mk() { sleep 300 & pids+=($!); }
mk; mine=$!; mk; reused=$!; mk; other=$!
mkdir "$T/run" "$T/reused" "$T/gone" "$T/empty"
proc_id "$mine" >"$T/run/run.pid"
echo "$reused 1" >"$T/reused/run.pid"
echo "999999 1" >"$T/gone/run.pid"
SIFT_CI_STOP_WAIT=5 "$REPO/scripts/ci.sh" stop "$T/run" >/dev/null 2>&1 || fail "stop of a live run exits non-zero"
sleep 0.3
kill -0 "$mine" 2>/dev/null && fail "the recorded process is still running"
kill -0 "$other" 2>/dev/null || fail "a process with the same command line, not recorded, was ended"
out=$("$REPO/scripts/ci.sh" stop "$T/reused" 2>&1); rc=$?
(( rc != 0 )) || fail "a recorded pid with another start time was accepted"
kill -0 "$reused" 2>/dev/null || fail "a reused pid was ended"
[[ $out == *"left alone"* ]] || fail "no word about the refusal: $out"
"$REPO/scripts/ci.sh" stop "$T/gone" 2>&1 | grep -q "is not running" || fail "a run that is gone is not reported"
"$REPO/scripts/ci.sh" stop "$T/empty" >/dev/null 2>&1 && fail "a directory with no record was accepted"
env -u SIFT_CI_TMP "$REPO/scripts/ci.sh" stop >/dev/null 2>&1 && fail "stop with no directory was accepted"
kill -0 "$other" 2>/dev/null || fail "a process was ended by one of the refusals"
exit "$bad"
