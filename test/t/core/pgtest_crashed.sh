#!/usr/bin/env bash
# tap: pooled
# scripts/pgtest.sh regress names the test whose process pg_regress reports as exited abnormally when a backend crashes in a batch, not the first test that only differs.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
plan 4

eval "$(sed -n '/^crashed_test()/p' "$SIFT_REPO/scripts/pgtest.sh")"
TMP=$(dir pgtest)
mkdir -p "$TMP/regress/crash" "$TMP/regress/diffs"
printf '%s\n' '# using postmaster on /tmp/sock, port 1' 'not ok 1     - agg/diff_only    12 ms' 'ok 2         - agg/passes    3 ms' \
	'not ok 3     - agg/crashes    9 ms' '# (test process exited with exit code 2)' 'not ok 4     - agg/after    1 ms' '1..4' >"$TMP/regress/crash/regression.out"
printf '%s\n' 'not ok 1     - agg/diff_only    12 ms' 'ok 2         - agg/passes    3 ms' '1..2' >"$TMP/regress/diffs/regression.out"

check "the test that exited abnormally is named" test "$(crashed_test crash)" = agg/crashes
check "not the first test that differs" test "$(crashed_test crash)" != agg/diff_only
check "no test is named when none exited abnormally" test -z "$(crashed_test diffs)"
check "no test is named without the output of the batch" test -z "$(crashed_test missing)"

finish
