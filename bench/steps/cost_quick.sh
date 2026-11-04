#!/usr/bin/env bash
# Usage: bench/steps/cost_quick.sh FILE   (a stage of `bench/bench release-quick`)
# The cost of the default `track_changes` on a write, at 100,000 rows: bench/churn/heapfree_default.py (the same statements on an index with the option off and with its default, one to
# three indexes) against a test cluster of this checkout on tmpfs (scripts/pgtest.sh), the records in FILE.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
out=${1:?usage: cost_quick.sh FILE}
export SIFT_TMP=${SIFT_TMP:-/dev/shm/sift-cost-quick} SIFT_PORT=${SIFT_PORT:-56261}
trap '"$REPO/scripts/pgtest.sh" destroy >/dev/null 2>&1 || true' EXIT
"$REPO/scripts/pgtest.sh" install >/dev/null
"$REPO/scripts/pgtest.sh" start >/dev/null
eval "$("$REPO/scripts/pgtest.sh" env)"
python3 -I "$BENCH/churn/heapfree_default.py" --host "$PGHOST" --port "$PGPORT" --out "$out" --rows "${COST_ROWS:-100000}" --reps 3
