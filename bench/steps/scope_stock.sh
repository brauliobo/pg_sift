#!/usr/bin/env bash
# Usage: bench/bench scope-stock [--size quick|standard] [--docs] [--arms LIST] [--tenants LIST] [--facts N] [--build DIR] [--root DIR] [--out FILE] [--keep] [--resume]   (steps/scope_stock.sh)
# The scope index against tuned stock PostgreSQL (steps/scope_stock_arms.py): nine read classes, eight tenants, warm and cold, eight arms (the b-tree and semi join as written, the tuned semi join,
# copies of the events by tenant kept by triggers, rollups, pg_ivm incremental views, the entity index, the scope index), every result md5-equal to the plain statement, and the write cost of each arm.
#   --size        quick (240,000 events, tenants 1, 4 and 8, five arms: minutes; the quick release profile runs it) or standard (2.4 million events, eight tenants, every arm; the release pass)
#   --docs        standard size only: also write the results file and rewrite the generated blocks of README.md and docs/COL-SCOPE.md (scripts/gen-docs.py)
#   --arms LIST   some arms only (stock_default is the oracle of every md5 and must be in the list); with --docs their rows replace those of the arms in the results file (merge)
#   --resume      after an interrupted --keep run: the arms already saved in its raw directory are not measured again (needs --keep both times)
#   --out FILE    the results file (default bench/out/scope-stock-DATE.tsv); --keep keeps the raw per-arm tables next to it
#   --build DIR   the checkout whose sift is measured (default: this one; its scripts/pgtest.sh stages the release library); --root DIR a staged install of it
# pg_ivm is the pinned release of bench/versions.env, built once by steps/fetch.sh into the engines directory of the work directory (BENCH_WORK); when it cannot be built the arm is skipped with a
# notice and the results file says so. Everything runs on a scratch cluster of its own on tmpfs under SCRATCH_ROOT (never an existing cluster), in a systemd scope (12G, 3 CPUs); the arms wait for a
# quiet host (CPU and IO pressure under 10%).
set -euo pipefail
export LC_ALL=C
HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=$(dirname "$HERE")
REPO=$(dirname "$BENCH")
. "$HERE/scratch.sh"
. "$BENCH/versions.env"

ORIG=("$@")
SIZE=standard DOCS='' RESUME='' OUT='' KEEP=${KEEP:-} ARGS=()
while (( $# )); do
	case $1 in
		--size) SIZE=$2; shift ;;
		--docs) DOCS=1 ;;
		--arms|--tenants|--facts) ARGS+=("$1" "$2"); shift ;;
		--build) SIFT_BUILD=$(cd "$2" && pwd); shift ;;
		--root) export SIFT_ROOT=$2; shift ;;
		--out) OUT=$2; shift ;;
		--keep) KEEP=1 ;;
		--resume) ARGS+=("$1"); RESUME=1 ;;
		*) sed -n '2,15p' "$0"; exit 1 ;;
	esac
	shift
done
[[ -z $DOCS || $SIZE == standard ]] || { echo "scope-stock: --docs publishes numbers: they are only measured at --size standard, not $SIZE" >&2; exit 1; }
scratch_scope "${ORIG[@]}"

mkdir -p "$BENCH/out"
OUT=${OUT:-$BENCH/out/scope-stock-$SIZE-$(date -u +%Y%m%d).tsv}
scratch_install scope-stock
scratch_port
RAW=$SIFT_TMP/raw
IVM=${BENCH_WORK:-${SIFT_BENCH_ROOT:-/tmp/sift-bench}}/engines/pg_ivm/root
cleanup() { [[ -n $KEEP ]] || rm -rf "$SIFT_TMP/data" "$SIFT_TMP/sock" "$SIFT_TMP/log" "$RAW"; }
trap cleanup EXIT
if [[ ! -d $IVM ]] && ! "$HERE/fetch.sh" pg_ivm >&2; then
	echo "NOTICE: scope-stock: pg_ivm $PG_IVM_VERSION could not be built (steps/fetch.sh pg_ivm); the ivm arm is skipped" >&2
fi
roots=(--root "$SIFT_ROOT"); [[ ! -d $IVM ]] || roots+=(--root "$IVM")

[[ -n $RESUME ]] || rm -rf "$SIFT_TMP/data" "$RAW"
mkdir -p "$RAW"
build=$(git -C "$SIFT_BUILD" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
python3 -I "$HERE/scope_stock_arms.py" run "${roots[@]}" --dir "$SIFT_TMP" --port "$SIFT_PORT" --out "$RAW" --size "$SIZE" --build-id "$build" "${ARGS[@]}"
python3 -I "$HERE/scope_stock_arms.py" collect "$RAW" --out "$OUT"
[[ -z $KEEP ]] || cp -r "$RAW" "${OUT%.tsv}.raw"
echo "scope-stock: $OUT" >&2
python3 -I "$HERE/scope_stock_arms.py" report "$OUT"
if [[ -n $DOCS ]]; then
	RESULTS=$REPO/docs/results/scope-stock.tsv
	if [[ " ${ARGS[*]} " == *" --arms "* ]]; then
		python3 -I "$HERE/scope_stock_arms.py" merge "$RESULTS" "$OUT" --out "$RESULTS.merged"
		mv "$RESULTS.merged" "$RESULTS"
	else
		cp "$OUT" "$RESULTS"
	fi
	python3 -I "$REPO/scripts/gen-docs.py"
fi
