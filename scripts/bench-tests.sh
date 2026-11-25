#!/usr/bin/env bash
# scripts/bench-tests.sh [FILE...]: bench/test/*.py (or FILEs), all at once (BENCH_TESTS_JOBS caps them, default the cores); a failing test prints its name, exit status and the end of its output, and the last line names them all
#   scripts/bench-tests.sh --selftest   a passing, a failing and a crashing test: both failures are named with their output, the passing one is silent, the run exits 1
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
cd "$(dirname "$0")/.." || exit 1
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
slots=${BENCH_TESTS_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}
run_tests() {
	local t running=0
	rm -f "$out/failed"
	for t in "$@"; do
		(( running < slots )) || { wait -n; running=$((running - 1)); }
		{ python3 -I "$t" >"$out/${t##*/}.log" 2>&1 || echo "$? $t" >>"$out/failed"; } &
		running=$((running + 1))
	done
	wait
	[[ -e $out/failed ]] || return 0
	sort -k2 "$out/failed" | while read -r rc t; do echo "FAIL $t (exit $rc)"; tail -n 12 "$out/${t##*/}.log"; done
	echo "failed: $(sort -k2 "$out/failed" | cut -d' ' -f2 | tr '\n' ' ')"
	return 1
}
if [[ ${1:-} == --selftest ]]; then
	mkdir "$out/t"
	echo 'print("fine")' >"$out/t/ok.py"
	echo 'import sys; print("assertion text of the failing test"); sys.exit(1)' >"$out/t/bad.py"
	echo 'raise RuntimeError("crash text of the crashing test")' >"$out/t/crash.py"
	res=$(run_tests "$out"/t/*.py); rc=$?
	[[ $rc == 1 ]] || { echo "FAIL: bench-tests exits $rc with two failing tests, want 1"; exit 1; }
	for want in "FAIL $out/t/bad.py (exit 1)" "assertion text of the failing test" "FAIL $out/t/crash.py (exit 1)" "crash text of the crashing test" "failed: $out/t/bad.py $out/t/crash.py "; do
		[[ $res == *"$want"* ]] || { echo "FAIL: bench-tests does not print: $want"; echo "$res"; exit 1; }
	done
	[[ $res != */t/ok.py* ]] || { echo "FAIL: bench-tests prints the passing test: $res"; exit 1; }
	run_tests "$out/t/ok.py" || { echo "FAIL: bench-tests fails a run of passing tests"; exit 1; }
	echo "bench-tests: a failing test is named with its exit status and output, a passing one is silent"
	exit 0
fi
(( $# )) || set -- bench/test/*.py
run_tests "$@"
