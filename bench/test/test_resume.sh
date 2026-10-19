#!/usr/bin/env bash
# The resume contract of bench/steps/lib.sh, checked by killing a run at random points: a synthetic run (stages of cells, rounds of cells.tsv) is started, killed with SIGKILL after a random time,
# resumed until it finishes, and its results are compared with an uninterrupted run: the same files, the same contents, no duplicate rows, no half-written file. Also: a killed run is found out
# as crashed by the next start, SIGTERM exits with 75 and records `interrupted`, and a run with another key starts over instead of mixing. Usage: bench/test/test_resume.sh [KILLS] (default 6)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
KILLS=${1:-6}
T=$(mktemp -d "${SIFT_TMP:-/tmp}/sift-resume-test.XXXXXX")
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

cat >"$T/work.sh" <<'WORK'
#!/usr/bin/env bash
# usage: work.sh OUTDIR KEYWORD: three stages of five cells, then a table of three rounds
set -euo pipefail
. "$LIBSH"
OUT=$1 KEY=$2 RUNS_DIR=$(dirname "$1")
export BENCH_RUN_ID=${OUT##*/run-}
mkdir -p "$OUT"
log() { :; }
run_begin work "$@"
cell() { # STAGE N: its result is kept once written; a cell that exists with this key is not measured again
  local f=$OUT/$1-$2.json
  [[ $(cat "$f" 2>/dev/null) == "{\"key\": \"$KEY\", \"cell\": \"$1-$2\"}" ]] && return 0
  sleep "${CELL_SLEEP:-0.03}"
  printf '{"key": "%s", "cell": "%s-%s"}' "$KEY" "$1" "$2" | atomic_write "$f"
}
st() { local n; for n in 1 2 3 4 5; do cell "$1" "$n"; done; }
for s in a b c; do stage_run "$s" "$KEY" 0 st "$s"; done
DEST=$OUT/table ENGINES=e CLASSES=c RSE=0.05 TIER=Q MINR=3 MAXR=3
mkdir -p "$DEST"
stable() { return 1; }
round() { local c; for c in x y z; do sleep 0.02; printf '%s\t0\t%s\t7\n' "$1" "$c" >>"$DEST/cells.tsv"; done; }
cells_begin $'round\tload\tclass\tvalue'
rounds round
WORK
chmod +x "$T/work.sh"
export LIBSH=$HERE/../steps/lib.sh

snapshot() { (cd "$1" && find . -type f ! -path './.run/*' -not -regex '.*\.[A-Za-z0-9]\{6\}' | sort | while read -r f; do echo "== $f"; cat "$f"; echo; done); }

"$T/work.sh" "$T/runs/run-clean" k1 >/dev/null 2>&1
snapshot "$T/runs/run-clean" >"$T/clean.snap"
[[ $(cut -f1 "$T/runs/run-clean/.run/state") == 'done' ]] || fail "the clean run is not done"
(( $(grep -c . "$T/runs/run-clean/table/cells.tsv") == 10 )) || fail "the clean table has not 9 rows and a header"

for i in $(seq "$KILLS"); do
  d=$T/runs/run-kill$i
  for attempt in $(seq 12); do   # killed at a random point, started again, until it finishes
    "$T/work.sh" "$d" k1 >/dev/null 2>&1 &
    pid=$!
    sleep "0.0$(( RANDOM % 9 + 1 ))$(( RANDOM % 10 ))"
    if kill -0 "$pid" 2>/dev/null; then kill -KILL "$pid"; wait "$pid" 2>/dev/null || true; else wait "$pid" && break; fi
  done
  [[ $(cut -f1 "$d/.run/state") == 'done' ]] || { "$T/work.sh" "$d" k1 >/dev/null 2>&1; }
  [[ -z ${VERBOSE:-} ]] || echo "run $i: killed $((attempt - 1)) times"
  [[ $(cut -f1 "$d/.run/state") == 'done' ]] || fail "run $i did not finish"
  snapshot "$d" >"$T/kill$i.snap"
  diff -q "$T/clean.snap" "$T/kill$i.snap" >/dev/null || { diff "$T/clean.snap" "$T/kill$i.snap" | head >&2; fail "run $i differs from the uninterrupted run"; }
done
echo "ok: $KILLS runs killed at random points and resumed equal the uninterrupted run"

# another key: nothing of the old key is reused
"$T/work.sh" "$T/runs/run-kill1" k2 >/dev/null 2>&1
grep -q '"key": "k2"' "$T/runs/run-kill1/a-1.json" || fail "a result of another key was kept"
(( $(grep -c . "$T/runs/run-kill1/table/cells.tsv") == 10 )) || fail "the table of another key was not started over"
echo "ok: a run with another key measures again"

# a killed run is found out as crashed and what it left running (orphans of the SIGKILL) is stopped; SIGTERM is resumable
d=$T/runs/run-orphan
CELL_SLEEP=30 "$T/work.sh" "$d" k5 >/dev/null 2>&1 & pid=$!
sleep 0.6; kill -KILL "$pid"; wait "$pid" 2>/dev/null || true
( . "$LIBSH"; run_procs orphan ) | grep -q . || fail "the orphan of the killed run is not found by its environment"
( . "$LIBSH"; log() { :; }; RUNS_DIR=$T/runs run_reclaim ) >/dev/null 2>&1
( . "$LIBSH"; run_procs orphan ) | grep -q . && fail "the orphan of the killed run is still running" || true
echo "ok: the processes a killed run left are stopped"

d=$T/runs/run-crash
"$T/work.sh" "$d" k3 >/dev/null 2>&1 & pid=$!
sleep 0.15; kill -KILL "$pid"; wait "$pid" 2>/dev/null || true
[[ $(cut -f1 "$d/.run/state") == running ]] || fail "a killed run should still say running"
( . "$LIBSH"; log() { :; }; RUNS_DIR=$T/runs run_reclaim ) >/dev/null 2>&1
[[ $(cut -f1 "$d/.run/state") == crashed ]] || fail "a run whose process is gone was not found out"
d=$T/runs/run-term
"$T/work.sh" "$d" k4 >/dev/null 2>&1 & pid=$!
sleep 0.15; kill -TERM "$pid"; rc=0; wait "$pid" || rc=$?
(( rc == 75 )) || fail "SIGTERM exit status is $rc, not 75"
[[ $(cut -f1 "$d/.run/state") == interrupted ]] || fail "SIGTERM did not record interrupted"
"$T/work.sh" "$d" k4 >/dev/null 2>&1
[[ $(cut -f1 "$d/.run/state") == 'done' ]] || fail "the interrupted run did not finish when resumed"
echo "ok: a crashed run is detected; SIGTERM exits 75 and the run resumes"
