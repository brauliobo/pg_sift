#!/usr/bin/env bash
# Self-test of scripts/dry-perf-gate.sh with known outcomes (serial, about 15 minutes): a comment-only patch must be PASS-IDENTICAL (and PASS with the workload forced),
# a small injected cost under the absolute floor must pass, a large one must FAIL. Usage: selftest.sh [BASE] (default HEAD of --src / GATE_SRC; the patches are cut from it).
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
G=$HERE/../dry-perf-gate.sh
SRC=${GATE_SRC:-$(cd "$HERE/../.." && pwd)}
BASE=$(git -C "$SRC" rev-parse --verify "${1:-HEAD}^{commit}") || exit 2
OUT=${GATE_STATE:-${XDG_STATE_HOME:-$HOME/.local/state}/sift-perf-gate}/selftest
mkdir -p "$OUT"
python3 -I - "$SRC" "$BASE" "$OUT" <<'PY'
import difflib, re, subprocess, sys
src, base, out = sys.argv[1:]
def show(f):
    return subprocess.run(['git', '-C', src, 'show', f'{base}:{f}'], capture_output=True, text=True, check=True).stdout
def mk(name, f, fn):
    a = show(f); b = fn(a)
    d = f'diff --git a/{f} b/{f}\n' + ''.join(difflib.unified_diff(a.splitlines(True), b.splitlines(True), f'a/{f}', f'b/{f}', n=3))
    open(f'{out}/{name}.patch', 'w').write(d)
def inj(n):
    def f(s):
        m = re.search(r'\nrun_scan\(AggExec \*ex\)\n\{\n', s)
        assert m, 'run_scan not found'
        return s[:m.end()] + f'\tvolatile int gate_i;\n\n\tfor (gate_i = 0; gate_i < {n}; gate_i++)\n\t\t;\n' + s[m.end():]
    return f
mk('noop-comment', 'src/agg/kern.c', lambda s: '/* gate self-test: a comment and a blank line move every later line down by two */\n\n' + s)
mk('inject-small', 'src/agg/exec/run.c', inj(2000))
mk('inject-large', 'src/agg/exec/run.c', inj(20000))
PY
t() { # NAME EXPECTED-EXIT ARGS...
	local name=$1 want=$2; shift 2
	echo "=== $name (expect exit $want)"
	"$G" run --src "$SRC" --json "$OUT/$name.json" "$@" > "$OUT/$name.log" 2>&1
	local rc=$?
	grep -E '^perf-gate|FAIL' "$OUT/$name.log" | cut -c1-260 | head -12
	echo "--- $name: exit $rc, expected $want: $([[ $rc == "$want" ]] && echo OK || echo MISMATCH)"
}
t noop-identical 0 --patch "$OUT/noop-comment.patch" "$BASE"
t noop-workload  0 --patch "$OUT/noop-comment.patch" --force-workload --groups agg-kernel "$BASE"
t inject-small   0 --patch "$OUT/inject-small.patch" --groups smoke "$BASE"
t inject-large   1 --patch "$OUT/inject-large.patch" --groups smoke "$BASE"
