#!/usr/bin/env bash
# Usage: bench/bench shred   (SHRED_DOCS orders, default 50000; SHRED_ROUNDS best-of rounds, default 3)
# The throughput of the shredder (docs/COL-SHRED.md) and of its SIMD kernels, on a throwaway cluster of scripts/pgtest.sh (the
# testing build: sift._test_shred_count and sift._test_shred_kern_rate), SIFT_TMP (default /tmp/sift-bench-shred) and SIFT_PORT
# (default 56190). The cluster is destroyed when the step ends. bench/bench holds the perf lock; the server must not inherit it.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(dirname "$(dirname "$HERE")")
export SIFT_TMP=${SIFT_TMP:-/tmp/sift-bench-shred} SIFT_PORT=${SIFT_PORT:-56190}
PGTEST=$REPO/scripts/pgtest.sh

trap '"$PGTEST" destroy >/dev/null 2>&1' EXIT
"$PGTEST" install >/dev/null
"$PGTEST" start 8>&- >/dev/null
echo "shredder throughput: ${SHRED_DOCS:-50000} orders, best of ${SHRED_ROUNDS:-3} rounds"
"$PGTEST" psql -X -q -v ndocs="${SHRED_DOCS:-50000}" -v rounds="${SHRED_ROUNDS:-3}" -f "$REPO/bench/sql/shred.sql" | column -t -s $'\t'
