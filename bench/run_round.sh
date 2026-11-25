#!/usr/bin/env bash
# bench/run_round.sh [--docs] [--verify-diff] [--scales N,N,N | --release-scale [N,N,N]] [WORKTREE [ROUND [PORT]]]
#
# The 6.0 core harness (bench/f2_harness/) on a throwaway cluster: builds pg_sift from WORKTREE (default: this checkout) in /tmp/sift-bench-build/ROUND, runs a cluster in /tmp/sift-bench-ROUND,
# measures the 10k -> 40k -> 160k scaling ladder (growth factors, memory flatness, compression, WAL, write amplification, OLTP, dirty state, Q1-Q8, soft delete, planning, idle memory)
# and removes the cluster. Nothing is written into WORKTREE. No Docker, no root. About 25 s of measurement at the default ladder.
#   --docs            also write docs/results/v6-core.tsv of this checkout (scripts/gen-docs.py reads it)
#   --release-scale   the sign-off ladder 10k, 100k, 1M (or the row counts given)
#   --verify-diff     compare the checksum of every Q1-Q8 and F4-F8 answer of the sift plan with the heap scan's (V_diff rows); a difference fails the run
# Output: bench/out/ROUND.tsv and .md, and the comparison with bench/baselines/v510.tsv in bench/out/ROUND-vs-v510.md (bench/compare_rounds.py)
set -euo pipefail
export LC_ALL=C

BENCH_DIR=$(cd "$(dirname "$0")" && pwd)
. "$BENCH_DIR/steps/f2/cluster.sh"

HARNESS_ARGS=()
while (( $# )); do
	case $1 in
		--docs) HARNESS_ARGS+=(--docs) ;;
		--verify-diff) HARNESS_ARGS+=(--verify-diff) ;;
		--scales) HARNESS_ARGS+=(--scales "$2"); shift ;;
		--release-scale) if [[ ${2:-} == [0-9]* ]]; then HARNESS_ARGS+=(--release-scale "$2"); shift; else HARNESS_ARGS+=(--release-scale); fi ;;
		-h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		--*) echo "run_round.sh: unknown option $1" >&2; exit 1 ;;
		*) break ;;
	esac
	shift
done
WORKTREE=${1:-$(dirname "$BENCH_DIR")}
ROUND=${2:-v6-core-$(date -u +%Y%m%dT%H%M%S)}
PORT=${3:-${SIFT_PORT:-57640}}
OUT_DIR=$BENCH_DIR/out

v6_cluster_init "$WORKTREE" "$ROUND" "$PORT"
t_total=$(date +%s.%N)
echo "[bench] round=$ROUND worktree=$WT_PATH port=$PORT pg=$PG_MAJOR"
v6_cluster_build
v6_cluster_start
mkdir -p "$OUT_DIR"

echo "[bench] running f2_harness"
t_bench=$(date +%s.%N)
PYTHONPATH="$BENCH_DIR" PYTHONDONTWRITEBYTECODE=1 nice -n 10 python3 -m f2_harness \
	--host "$SOCK_DIR" --port "$PORT" --db bench --user postgres \
	--round "$ROUND" --git-sha "$GIT_SHA" --git-branch "$GIT_BRANCH" --build-sec "$BUILD_SEC" --make-jobs "$MAKE_JOBS" \
	--out-dir "$OUT_DIR" ${HARNESS_ARGS[@]+"${HARNESS_ARGS[@]}"}
now=$(date +%s.%N)
echo "[bench] done: measurement $(awk -v a="$now" -v b="$t_bench" 'BEGIN { printf "%.1f", a - b }')s, total $(awk -v a="$now" -v b="$t_total" 'BEGIN { printf "%.1f", a - b }')s"
echo "[bench] outputs: $OUT_DIR/$ROUND.tsv and $OUT_DIR/$ROUND.md"

if [[ -f $BENCH_DIR/baselines/v510.tsv ]]; then
	python3 "$BENCH_DIR/compare_rounds.py" "$BENCH_DIR/baselines/v510.tsv" "$OUT_DIR/$ROUND.tsv" -o "$OUT_DIR/$ROUND-vs-v510.md"
	sed -n '5,6p' "$OUT_DIR/$ROUND-vs-v510.md"
	echo "[bench] comparison: $OUT_DIR/$ROUND-vs-v510.md"
fi
