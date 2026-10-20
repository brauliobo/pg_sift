#!/usr/bin/env bash
# The timed runs of the set filter benchmarks (run.py): in a systemd scope that caps the host share (the lock /tmp/sift-perf.lock is taken by run.py).
#   bench/sets/run.sh OUT.tsv [run.py options]
exec systemd-run --user --scope -q -p MemoryMax=12G -p CPUQuota=300% python3 "$(dirname "$0")/run.py" "$@"
