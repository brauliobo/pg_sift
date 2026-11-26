#!/bin/bash
# unit-scaling.sh [LOG]: builds every test and benchmark of test/unit under ASan and UBSan and runs every test, four at a time (make -C test/unit MODE=asan
# all test), the log in LOG (default stdout); the calls a Format 2 test makes into the server are stubs that abort, so a test that reaches one fails.
# OUT=DIR builds in DIR and keeps it (an unchanged test is not built again), else in a new directory removed at exit.
set -euo pipefail
export LC_ALL=C
REPO=$(cd "$(dirname "$0")/.." && pwd)
LOG=${1:-/dev/stdout}
if [[ -z ${OUT:-} ]]; then
	OUT=$(mktemp -d "${TMPDIR:-/tmp}/sift-unit-XXXXXX")
	trap 'rm -rf "$OUT"' EXIT
fi
nice make -s -k -O -j4 -C "$REPO/test/unit" MODE=asan OUT="$OUT" all test >"$LOG" 2>&1
