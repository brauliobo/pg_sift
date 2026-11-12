#!/usr/bin/env bash
# The focused benches of bench/focus on one staged install (make install DESTDIR=ROOT): minutes, one scratch cluster of about 300 MB on tmpfs per case.
#
#   bench/focus/run.sh wamp  ROOT LABEL    write amplification, cases std-ck24 std-ck1 wide-ck24, 48 batches each (about 1.5 minutes a case): table, then the check against expect-wamp.tsv
#   bench/focus/run.sh reads ROOT LABEL    read cost per class, cases std and wide, 54 batches each (about 2.5 minutes a case): table
#   bench/focus/run.sh both  ROOT LABEL
#   bench/focus/run.sh expect LABEL...     the expectation rows of the wamp results of these labels (two runs of one build give the tolerance)
#
# Results go to $FOCUS_OUT (default $FOCUS_SCRATCH/out): wamp-CASE-ckN-LABEL.json|txt, reads-CASE-LABEL.json|txt. Compare two builds: bench/focus/compare.py 'out/reads-std-A*.json' 'out/reads-std-B*.json'.
# PARALLEL=1 runs the cases at once (one cluster each, ports from FOCUS_PORT). FOCUS_ROWS, FOCUS_N and the others are in lab.py.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
export LC_ALL=C PGHOST=/nonexistent PGPORT=1
export FOCUS_SCRATCH=${FOCUS_SCRATCH:-/dev/shm/sift-focus}
OUT=${FOCUS_OUT:-$FOCUS_SCRATCH/out}
WORK=${FOCUS_WORK:-$FOCUS_SCRATCH/work}
mkdir -p "$OUT"
PY="nice python3 -I"
base_port=${FOCUS_PORT:-56350}

cases() { # CMD ARGS...: the command for each case, in turn or at once
	local i=0 pids=() spec
	for spec in "${CASES[@]}"; do
		[[ -d $WORK/${spec%%:*}/data ]] || FOCUS_PORT=$((base_port + i)) $PY "$HERE/lab.py" base "${spec%%:*}" "$WORK" > /dev/null
		if [[ -n ${PARALLEL:-} ]]; then FOCUS_PORT=$((base_port + i)) "$@" "$spec" & pids+=($!); else FOCUS_PORT=$base_port "$@" "$spec"; fi
		i=$((i + 1))
	done
	for p in "${pids[@]}"; do wait "$p"; done
}

run_wamp() { # SPEC = case:checkpoint cadence
	$PY "$HERE/wamp.py" run "${1%%:*}" "${1##*:}" "$WORK" "$ROOT" "$OUT/wamp-${1%%:*}-ck${1##*:}-$LABEL" > /dev/null
}

run_reads() {
	$PY "$HERE/reads.py" run "${1%%:*}" "$WORK" "$ROOT" "$OUT/reads-${1%%:*}-$LABEL" > /dev/null
}

mode=${1:?usage: run.sh wamp|reads|both ROOT LABEL | expect LABEL...}
if [[ $mode == expect ]]; then
	shift
	files=()
	for l in "$@"; do files+=("$OUT"/wamp-*-"$l".json); done
	exec $PY "$HERE/wamp.py" expect "${files[@]}"
fi
ROOT=${2:?ROOT} LABEL=${3:?LABEL}
if [[ $mode == wamp || $mode == both ]]; then
	CASES=(std:24 std:1 wide:24)
	cases run_wamp
	files=("$OUT"/wamp-*-"$LABEL".json)
	$PY "$HERE/wamp.py" table "${files[@]}"
	$PY "$HERE/wamp.py" check "$HERE/expect-wamp.tsv" "${files[@]}" || [[ -n ${FOCUS_NO_CHECK:-} ]]
fi
if [[ $mode == reads || $mode == both ]]; then
	CASES=(std wide)
	cases run_reads
	$PY "$HERE/reads.py" table "$OUT"/reads-std-"$LABEL".json
	$PY "$HERE/reads.py" table "$OUT"/reads-wide-"$LABEL".json
fi
