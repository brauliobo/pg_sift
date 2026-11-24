#!/usr/bin/env bash
# `scripts/pgtest.sh regress` with a batch of four tests names the guilty test with the fewest runs: a clean batch is one run, a crashed backend stops at once and names its test, a
# diff runs only the tests that differ alone, and only a leak or a leftover (or tests that pass alone and fail together) cuts the batch in halves; SIFT_REGRESS_NO_BISECT=1 never runs a
# test again. pg_regress, the server and its tools are stubs of this script (no PostgreSQL needed, a few seconds): the stub pg_regress logs every run, and a scenario says which tests
# differ, crash, or leak when they run together.
#
#   scripts/selftest-pgtest-bisect.sh
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d "${SIFT_TMP:-${TMPDIR:-/tmp}}/pgtest-bisect.XXXXXX")
bin=$tmp/bin calls=$tmp/calls out=$tmp/out
export SIFT_TMP=$tmp/cluster SIFT_PORT=${SIFT_PORT:-56100} STUB_CALLS=$calls STUB_LOG=$tmp/cluster/postgres.log PATH=$bin:$PATH
fail=0 pm=
trap '[[ -z $pm ]] || kill "$pm" 2>/dev/null; rm -rf "$tmp"' EXIT

mkdir -p "$bin" "$SIFT_TMP/data" "$SIFT_TMP/sock" "$tmp/pgxs/src/makefiles" "$tmp/pgxs/src/test/regress"
: >"$STUB_LOG"
sleep 300 & pm=$!
echo "$pm" >"$SIFT_TMP/data/postmaster.pid"
echo 17 >"$SIFT_TMP/data/PG_VERSION"

cat >"$bin/pg_config" <<EOF
#!/usr/bin/env bash
case \$1 in --version) echo "PostgreSQL 17.0" ;; --pgxs) echo "$tmp/pgxs/src/makefiles/pgxs.mk" ;; *) echo /stub ;; esac
EOF
printf '#!/usr/bin/env bash\nexit 0\n' | tee "$bin/pg_isready" "$bin/psql" "$bin/dropdb" >/dev/null

cat >"$tmp/pgxs/src/test/regress/pg_regress" <<'EOF'
#!/usr/bin/env bash
tests=()
for a; do case $a in --outputdir=*) out=${a#*=} ;; --*) ;; *) tests+=("$a") ;; esac; done
echo "${tests[*]}" >>"$STUB_CALLS"
has() { [[ " $1 " == *" $2 "* ]]; }
rc=0 n=0 crashed=
: >"$out/regression.out"; : >"$out/regression.diffs"
for t in "${tests[@]}"; do
	n=$((n + 1)) bad=
	if [[ -n $crashed ]]; then bad=1
	elif has "${STUB_CRASH:-}" "$t"; then
		crashed=1 bad=1
		if [[ ${STUB_CRASH_KIND:-trap} == trap ]]; then echo "TRAP: failed Assert(\"x == 1\"), File: \"a.c\", Line: 1, PID: 42" >>"$STUB_LOG"
		else echo "LOG:  server process (PID 42) was terminated by signal 6: Aborted" >>"$STUB_LOG"; fi
	elif has "${STUB_FAIL:-}" "$t" || { has "${STUB_BLEED:-}" "$t" && (( ${#tests[@]} > 1 )); }; then bad=1
		printf 'diff -U3 /in/expected/%s.out /out/results/%s.out\n--- a\n+++ b\n@@ -1 +1 @@\n-1\n+2\n' "$t" "$t" >>"$out/regression.diffs"
	fi
	if [[ -n $bad ]]; then rc=1; printf 'not ok %d     - %-20s 5 ms\n' "$n" "$t" | tee -a "$out/regression.out"
	else printf 'ok %d         - %-20s 5 ms\n' "$n" "$t" | tee -a "$out/regression.out"; fi
done
if [[ -n ${STUB_TOGETHER:-} ]]; then
	together=1
	for t in $STUB_TOGETHER; do has "${tests[*]}" "$t" || together=
	done
	[[ -z $together ]] || echo 'WARNING:  buffer refcount leak: [123] (rel=base/5/1259, blockNum=0, flags=0x93800000, refcount=1 1)' >>"$STUB_LOG"
fi
exit $rc
EOF
chmod +x "$bin"/* "$tmp/pgxs/src/test/regress/pg_regress"

# run LABEL VAR=VALUE...: regress of t1..t4 under the scenario VAR=VALUE...; $out is its output, $rc its status, $runs the pg_regress runs it took
run() {
	label=$1
	shift
	: >"$calls"
	env "$@" "$REPO/scripts/pgtest.sh" regress t1 t2 t3 t4 >"$out" 2>&1 && rc=0 || rc=$?
	runs=$(wc -l <"$calls")
}
check() { # LABEL CONDITION...: fails the selftest, with the output of the run, when the command fails
	local what=$1
	shift
	"$@" || { echo "FAIL: $label: $what"; sed 's/^/    /' "$out" | tail -n 12; fail=1; }
}
says() { grep -qE -e "$1" "$out"; }
silent() { ! grep -qE -e "$1" "$out"; }
runs_are() { [[ $runs == "$1" ]]; }
red() { (( rc != 0 )); }
green() { (( rc == 0 )); }
ran_alone() { grep -qx -- "$1" "$calls"; }
never_ran_alone() { ! ran_alone "$1"; }

cut='; cutting (it|the batch) in halves'

run clean A=1
check 'a clean batch passes' green
check 'a clean batch is one run' runs_are 1
check 'a clean batch does not cut' silent "$cut"

for kind in trap signal; do
	run "crash ($kind)" STUB_CRASH=t2 STUB_CRASH_KIND=$kind
	check 'a crash is red' red
	check 'a crash is one run: no half and no test alone' runs_are 1
	check 'a crash does not cut' silent "$cut"
	check 'a crash names the test that crashed the server' says 'Crashed in: t2; failed after it: t3 t4'
	check 'a crash shows the finding of the log' says 'leak in .*(TRAP:|terminated by signal 6)'
done

run 'one diff' STUB_FAIL=t3
check 'a diff is red' red
check 'a diff runs the batch and the test that differs: two runs' runs_are 2
check 'the test that differs runs alone' ran_alone t3
check 'the tests that pass do not run alone' never_ran_alone t1
check 'the diff is named' says 'diff .*/expected/t3.out'
check 'a diff does not cut' silent "$cut"

run 'two diffs' STUB_FAIL='t2 t4'
check 'two diffs are three runs' runs_are 3
check 'both tests that differ run alone' ran_alone t2
check 'both tests that differ run alone (t4)' ran_alone t4
check 'the tests that pass do not run alone (t1)' never_ran_alone t1
check 'the tests that pass do not run alone (t3)' never_ran_alone t3

run 'diff only together' STUB_BLEED=t3
check 'a test that passes alone and fails in the batch is red' red
check 'it runs alone first' ran_alone t3
check 'then the batch is cut: an earlier test bleeds into it' says 'fail in the batch and pass alone; cutting the batch in halves'
check 'the cut runs the halves' ran_alone 't1 t2'

run 'leak together' STUB_TOGETHER='t1 t2'
check 'a leak is red' red
check 'a leak cuts the batch in halves' says "$cut"
check 'a leak runs the halves' ran_alone 't3 t4'
check 'a leak of tests that pass alone says it is the tests together' says 'no half of the batch fails alone'

run 'leak together, no bisect' STUB_TOGETHER='t1 t2' SIFT_REGRESS_NO_BISECT=1
check 'a leak is red without the cuts' red
check 'a leak is one run without the cuts' runs_are 1
check 'a leak does not cut without the cuts' silent "$cut"
check 'a leak says it does not cut' says 'SIFT_REGRESS_NO_BISECT: not cutting'
check 'a leak shows its finding without the cuts' says 'buffer refcount leak'

run 'diff, no bisect' STUB_FAIL=t3 SIFT_REGRESS_NO_BISECT=1
check 'a diff is red without the cuts' red
check 'a diff is one run without the cuts' runs_are 1
check 'a diff names its test without the cuts' says 'failed in the batch: t3'

run 'crash, no bisect' STUB_CRASH=t2 SIFT_REGRESS_NO_BISECT=1
check 'a crash is red without the cuts' red
check 'a crash is one run without the cuts' runs_are 1
check 'a crash names its test without the cuts' says 'Crashed in: t2'

run 'bleed, no bisect' STUB_BLEED=t3 SIFT_REGRESS_NO_BISECT=1
check 'a bleed is red without the cuts' red
check 'a bleed is one run without the cuts' runs_are 1

(( fail == 0 )) && echo "pgtest bisect ok: a clean batch is one run, a crash stops at once, a diff runs only the tests that differ, a leak cuts unless SIFT_REGRESS_NO_BISECT"
exit $fail
