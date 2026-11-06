#!/usr/bin/env bash
# dry-perf-gate.sh [run|baseline|select|list] [options] BASE [CAND]: the performance gate of the DRY refactor commits (scripts/dry-perf-gate/USAGE.md, dry-perf-gate.py --help).
# GATE_HEAVY=CMD runs it behind a shared job queue wrapper (CMD [-t SECONDS] -- command); without it it runs at nice 19. Scratch only on tmpfs (GATE_ROOT, default /dev/shm/sift-perf-gate);
# baselines and noise floors under GATE_STATE (default $XDG_STATE_HOME/sift-perf-gate), never in the repository.
# Exit 0 PASS / PASS-IDENTICAL, 1 FAIL (a verdict on the commit), 2 infrastructure error (a damaged data directory, a library that does not match, a tool that did not run: no verdict, run it again), 3 the candidate does not build.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# a psql without a cluster of ours must never reach the system server
export PGHOST=/nonexistent PGPORT=1 LC_ALL=C
unset PGDATABASE PGSERVICE PGSERVICEFILE
if [[ -n ${GATE_HEAVY:-} && ${GATE_IN_SLOT:-} != 1 ]]; then
	export GATE_IN_SLOT=1
	exec "$GATE_HEAVY" -t "${GATE_TIMEOUT:-2400}" -- "$0" "$@"
fi
exec nice -n 19 python3 -I "$HERE/dry-perf-gate.py" "$@"
