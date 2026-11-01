#!/usr/bin/env bash
# The quick tier runs the scaling gate (tap-shell-scaling_slope) only for the files whose geometry the 8-page flavour changes: every source that names SIFT_SB_SHIFT must be
# in the row of scripts/areas.tsv that picks it, so that a change of the layout is never merged without the gate.
set -uo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
source "$REPO/scripts/areas.sh"
areas_load "$REPO/scripts/areas.tsv"
bad=0
while IFS= read -r f; do
	re=$(areas_regex "$f") || exit 2
	[[ tap-shell-scaling_slope =~ $re ]] || { echo "FAIL: $f names SIFT_SB_SHIFT and no row of scripts/areas.tsv picks tap-shell-scaling_slope for it"; bad=1; }
done < <(cd "$REPO" && git grep -l SIFT_SB_SHIFT -- src Makefile scripts/harden.mk)
(( bad )) || echo "scaling-triggers: every file that names SIFT_SB_SHIFT picks the scaling gate"
exit "$bad"
