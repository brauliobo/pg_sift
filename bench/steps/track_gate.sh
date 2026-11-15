#!/usr/bin/env bash
# The budget of track_changes for the writer (bench/steps/track_gate.py): starts a release cluster on tmpfs with scripts/pgtest.sh, runs the gate, removes the cluster.
# Standalone, run directly; not part of make check or scripts/ci.sh.
#
#   bench/steps/track_gate.sh [--rows N] [--reps N] [--mass N] [--json FILE]
set -Eeuo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export SIFT_TMP=${SIFT_GATE_TMP:-/dev/shm}/sift-track-gate SIFT_PORT=${SIFT_PORT:-56130} SIFT_TESTING=
trap '"$ROOT/scripts/pgtest.sh" destroy >/dev/null 2>&1 || true' EXIT
"$ROOT/scripts/pgtest.sh" install >/dev/null
"$ROOT/scripts/pgtest.sh" start >/dev/null
eval "$("$ROOT/scripts/pgtest.sh" env)"
python3 -I "$ROOT/bench/steps/track_gate.py" "$@"
