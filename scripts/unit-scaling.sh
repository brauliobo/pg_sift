#!/bin/bash
# unit-scaling.sh [LOG]: builds and runs every test of test/unit under ASan and UBSan (make -C test/unit run-asan), the log in LOG (default stdout); the
# calls a Format 2 test makes into the server are stubs that abort, so a test that reaches one fails
set -euo pipefail
export LC_ALL=C
REPO=$(cd "$(dirname "$0")/.." && pwd)
LOG=${1:-/dev/stdout}
OUT=$(mktemp -d "${TMPDIR:-/tmp}/sift-unit-XXXXXX")
trap 'rm -rf "$OUT"' EXIT
nice make -s -j4 -C "$REPO/test/unit" MODE=asan OUT="$OUT" all >"$LOG" 2>&1
nice make -s -C "$REPO/test/unit" OUT="$OUT" run-asan >>"$LOG" 2>&1
