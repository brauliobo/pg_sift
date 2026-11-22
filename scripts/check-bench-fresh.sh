#!/usr/bin/env bash
# Are the generated benchmark blocks and charts of README.md and docs/ what the renderers make of docs/results, and how old are the results?
# No benchmark runs: scripts/gen-docs.py --check renders into a scratch copy and compares it (python only, about a second).
#
#   scripts/check-bench-fresh.sh            exit 1 when a block or a chart differs from what scripts/gen-docs.py generates;
#                                           "warning:" lines on stdout, exit 0, when docs/results/v6-core.tsv or v6-scaling.tsv were measured at another MAJOR.MINOR than pg_sift.control
#   scripts/check-bench-fresh.sh --strict   the warnings fail too
# Fix: python3 scripts/gen-docs.py when only the generators or the text changed; bench/bench v6-release --docs for stale results.
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
STRICT='' warn=0
[[ ${1:-} != --strict ]] || STRICT=1

python3 -B -I "$R/scripts/gen-docs.py" --check

now=$(sed -n "s/^default_version *= *'\([0-9]*\.[0-9]*\).*/\1/p" "$R/pg_sift.control")
for f in v6-core v6-scaling; do
	[[ -f $R/docs/results/$f.tsv ]] || continue
	ver=$(sed -n 's/^# version=\([0-9]*\.[0-9]*\).*/\1/p' "$R/docs/results/$f.tsv" | head -1)
	[[ $ver == "$now" ]] || { echo "warning: docs/results/$f.tsv was measured at pg_sift ${ver:-an unknown version}, this tree is $now: bench/bench v6-release --docs"; warn=1; }
done
[[ -z $STRICT || $warn == 0 ]] || exit 1
echo "bench-fresh: the generated blocks equal what docs/results generates"
