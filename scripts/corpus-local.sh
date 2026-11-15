#!/usr/bin/env bash
# The corpus gate on a throwaway cluster: installs the library, starts a cluster, runs scripts/corpus.sh with the arguments, removes the cluster (test/corpus/README.md).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
export SIFT_TMP=${SIFT_TMP:-/tmp/sift-corpus-$$}
trap '"$REPO/scripts/pgtest.sh" destroy >/dev/null 2>&1 || true; rm -f "$SIFT_TMP.install.log"' EXIT
"$REPO/scripts/pgtest.sh" install >"$SIFT_TMP.install.log" 2>&1 || { tail -n 20 "$SIFT_TMP.install.log" >&2; exit 1; }
"$REPO/scripts/pgtest.sh" start >/dev/null
"$REPO/scripts/corpus.sh" "$@"
