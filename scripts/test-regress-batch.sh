#!/usr/bin/env bash
# `scripts/pgtest.sh regress` runs a batch of tests in one pg_regress run with one leak and one leftover check, and names the guilty test when the batch fails: a batch
# of 20 tests with one that leaves a table behind, one whose output differs and one that leaves a table another test creates must each name exactly those tests,
# and a clean batch passes; a batch of many reds is cut only until SIFT_BISECT_REDS of them are named. The tests are small fixtures in a directory of their own (SIFT_REGRESS_DIR) on a cluster of this script's own.
#
#   scripts/test-regress-batch.sh        (SIFT_PORT in 56100-56299, default 56100 + 7)
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/sift-regress-batch.XXXXXX")
export SIFT_TMP=$tmp/cluster SIFT_ROOT=$tmp/root SIFT_PORT=$(( ${SIFT_PORT:-56100} + 7 )) SIFT_REGRESS_DIR=$tmp/in SIFT_TESTING=
pg=$REPO/scripts/pgtest.sh
trap '"$pg" stop >/dev/null 2>&1; rm -rf "$tmp"' EXIT
fail=0

mkdir -p "$SIFT_REGRESS_DIR/sql" "$SIFT_REGRESS_DIR/expected" "$SIFT_ROOT"
echo "testing" >"$SIFT_ROOT/.flavor"
names=()
for i in $(seq -w 1 20); do names+=("t$i"); done
for t in "${names[@]}"; do
	printf 'CREATE TABLE tbl_%s (a int);\nINSERT INTO tbl_%s VALUES (1);\nSELECT a FROM tbl_%s;\nDROP TABLE tbl_%s;\n' "$t" "$t" "$t" "$t" >"$SIFT_REGRESS_DIR/sql/$t.sql"
	printf 'CREATE TABLE tbl_%s (a int);\nINSERT INTO tbl_%s VALUES (1);\nSELECT a FROM tbl_%s;\n a \n---\n 1\n(1 row)\n\nDROP TABLE tbl_%s;\n' "$t" "$t" "$t" "$t" >"$SIFT_REGRESS_DIR/expected/$t.out"
done
"$pg" start || { echo "FAIL: the cluster does not start"; exit 1; }

expect() { # LABEL WANT: the output of the run names WANT and no other test of the batch
	local label=$1 want=$2 t
	grep -q "$want" "$tmp/out" || { echo "FAIL: $label: no mention of $want"; sed 's/^/    /' "$tmp/out" | tail -n 20; fail=1; }
	for t in "${names[@]}"; do
		[[ $t == "${want#*: }" || $t == "$want" ]] && continue
		! grep -qE "(LEFTOVER|not ok - )$t( |:)" "$tmp/out" || { echo "FAIL: $label: $t is named and is not guilty"; fail=1; }
	done
}

"$pg" regress "${names[@]}" >"$tmp/out" 2>&1 || { echo "FAIL: a clean batch of 20 failed"; tail -n 20 "$tmp/out"; fail=1; }
runs=$(grep -c 'using postmaster' "$tmp/out")
[[ $runs == 1 ]] || { echo "FAIL: a clean batch took $runs pg_regress runs, want 1"; fail=1; }

# t13 leaves a table behind
printf 'CREATE TABLE leaked_by_t13 (a int);\n' >>"$SIFT_REGRESS_DIR/sql/t13.sql"
printf 'CREATE TABLE leaked_by_t13 (a int);\n' >>"$SIFT_REGRESS_DIR/expected/t13.out"
"$pg" regress "${names[@]}" >"$tmp/out" 2>&1 && { echo "FAIL: a batch with a leftover passed"; fail=1; }
grep -q 'LEFTOVER t13: .*leaked_by_t13' "$tmp/out" || { echo "FAIL: the leftover is not attributed to t13"; tail -n 20 "$tmp/out"; fail=1; }
expect leftover 't13'
printf 'CREATE TABLE tbl_t13 (a int);\nINSERT INTO tbl_t13 VALUES (1);\nSELECT a FROM tbl_t13;\nDROP TABLE tbl_t13;\n' >"$SIFT_REGRESS_DIR/sql/t13.sql"
sed -i '/leaked_by_t13/d' "$SIFT_REGRESS_DIR/expected/t13.out"

# t07 prints something else than its expected output
printf 'SELECT 2 AS two;\n' >>"$SIFT_REGRESS_DIR/sql/t07.sql"
printf 'SELECT 3 AS two;\n two \n-----\n   3\n(1 row)\n\n' >>"$SIFT_REGRESS_DIR/expected/t07.out"
"$pg" regress "${names[@]}" >"$tmp/out" 2>&1 && { echo "FAIL: a batch with a diff passed"; fail=1; }
grep -q 't07' "$tmp/out" || { echo "FAIL: the diff is not attributed to t07"; tail -n 20 "$tmp/out"; fail=1; }
expect diff 't07'
grep -q 't07' "$SIFT_TMP/regress/t01/regression.diffs" || { echo "FAIL: the first pass of the failed batch lost its regression.diffs to the cuts"; fail=1; }
cuts=$("$pg" psql -d postgres -qAt -c "SELECT count(*) FROM pg_database WHERE datname LIKE 'contrib_regression_t01_%'")
[[ $cuts == 0 ]] || { echo "FAIL: the cuts of the failed batch left $cuts databases in the cluster"; fail=1; }
[[ -e $SIFT_TMP/regress/cut/t07-1.report && ! -e $SIFT_TMP/regress/cut/t01-10.report ]] || { echo "FAIL: a diff runs only the test that differs alone, not the halves of the batch: $(ls "$SIFT_TMP/regress/cut" | tr "\n" " ")"; fail=1; }
printf 'CREATE TABLE tbl_t07 (a int);\nINSERT INTO tbl_t07 VALUES (1);\nSELECT a FROM tbl_t07;\nDROP TABLE tbl_t07;\n' >"$SIFT_REGRESS_DIR/sql/t07.sql"
printf 'CREATE TABLE tbl_t07 (a int);\nINSERT INTO tbl_t07 VALUES (1);\nSELECT a FROM tbl_t07;\n a \n---\n 1\n(1 row)\n\nDROP TABLE tbl_t07;\n' >"$SIFT_REGRESS_DIR/expected/t07.out"

# t05 leaves the table t06 creates: t06 fails in the batch, and alone it passes; the leftover is t05's
printf 'CREATE TABLE tbl_t06 (a int);\n' >>"$SIFT_REGRESS_DIR/sql/t05.sql"
printf 'CREATE TABLE tbl_t06 (a int);\n' >>"$SIFT_REGRESS_DIR/expected/t05.out"
"$pg" regress "${names[@]}" >"$tmp/out" 2>&1 && { echo "FAIL: a batch with a test that leaves another's table passed"; fail=1; }
grep -q 'LEFTOVER t05: .*tbl_t06' "$tmp/out" || { echo "FAIL: the leftover is not attributed to t05"; tail -n 20 "$tmp/out"; fail=1; }
! grep -qE '^not ok - t06|LEFTOVER t06' "$tmp/out" || { echo "FAIL: t06 is blamed for the table t05 left"; fail=1; }

# six tests print something else: the cuts name SIFT_BISECT_REDS of them and stop, the batch is red, and the tests they did not run alone are listed
printf 'CREATE TABLE tbl_t05 (a int);\nINSERT INTO tbl_t05 VALUES (1);\nSELECT a FROM tbl_t05;\nDROP TABLE tbl_t05;\n' >"$SIFT_REGRESS_DIR/sql/t05.sql"
printf 'CREATE TABLE tbl_t05 (a int);\nINSERT INTO tbl_t05 VALUES (1);\nSELECT a FROM tbl_t05;\n a \n---\n 1\n(1 row)\n\nDROP TABLE tbl_t05;\n' >"$SIFT_REGRESS_DIR/expected/t05.out"
for t in t02 t04 t06 t08 t10 t12; do
	printf 'SELECT 2 AS two;\n' >>"$SIFT_REGRESS_DIR/sql/$t.sql"
	printf 'SELECT 3 AS two;\n two \n-----\n   3\n(1 row)\n\n' >>"$SIFT_REGRESS_DIR/expected/$t.out"
done
SIFT_BISECT_REDS=2 "$pg" regress "${names[@]}" >"$tmp/out" 2>&1 && { echo "FAIL: a batch with six diffs passed"; fail=1; }
named=$(grep -cE '^diff .*/expected/t[0-9]+\.out' "$tmp/out")
[[ $named == 2 ]] || { echo "FAIL: the cuts named $named tests with SIFT_BISECT_REDS=2, want 2"; tail -n 20 "$tmp/out"; fail=1; }
grep -q 'not cutting further; not run alone: ' "$tmp/out" || { echo "FAIL: the cuts that stopped do not list the tests they did not run alone"; tail -n 20 "$tmp/out"; fail=1; }
named=$("$pg" regress "${names[@]}" 2>&1 | grep -cE '^diff .*/expected/t[0-9]+\.out')
[[ $named == 4 ]] || { echo "FAIL: the cuts named $named tests at the default SIFT_BISECT_REDS, want 4"; fail=1; }
for t in t02 t04 t06 t08 t10 t12; do
	printf 'CREATE TABLE tbl_%s (a int);\nINSERT INTO tbl_%s VALUES (1);\nSELECT a FROM tbl_%s;\nDROP TABLE tbl_%s;\n' "$t" "$t" "$t" "$t" >"$SIFT_REGRESS_DIR/sql/$t.sql"
	printf 'CREATE TABLE tbl_%s (a int);\nINSERT INTO tbl_%s VALUES (1);\nSELECT a FROM tbl_%s;\n a \n---\n 1\n(1 row)\n\nDROP TABLE tbl_%s;\n' "$t" "$t" "$t" "$t" >"$SIFT_REGRESS_DIR/expected/$t.out"
done

# the server is gone (the watchdog of the run stopped it at its cap): the cut says so once and stops, it does not run every half into a refused connection
"$pg" stop >/dev/null 2>&1
SIFT_READY_SECS=2 "$pg" regress "${names[@]:0:8}" >"$tmp/out" 2>&1 && { echo "FAIL: a batch on a stopped server passed"; fail=1; }
gone=$(grep -c 'the server is gone' "$tmp/out")
[[ $gone == 1 ]] || { echo "FAIL: a batch on a stopped server said $gone times that the server is gone, want 1"; tail -n 20 "$tmp/out"; fail=1; }
refused=$(grep -c 'No such file' "$tmp/out")
(( refused <= 12 )) || { echo "FAIL: a batch on a stopped server printed $refused refused connections"; fail=1; }

(( fail == 0 )) && echo "regress batch ok: a clean batch is one run, a leftover, a diff and a table left to another test are each attributed to their test"
exit $fail
