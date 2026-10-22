#!/usr/bin/env bash
# Usage: bench/bench scaling [--docs] [--ref TAG | --build DIR --root DIR] [--rows N] [--reps N] [--partitions N,N] [--workers N,N] [--out FILE] [--keep]   (steps/scaling.sh)
# Aggregates by number of partitions (1 to 4096, a hash of the id) and parallel workers (0 to 3) on the orders of the store benchmark, pg_sift against the normal plan of
# the same server (steps/scaling.py). Everything runs on a scratch cluster of its own under $SCRATCH_ROOT in a systemd scope (SCRATCH_MEM 12G, SCRATCH_CPU 400% so that 3
# workers and the leader have a CPU each); the timed groups of cells take PERF_LOCK (default /tmp/sift-perf.lock), and the host load is recorded with every cell.
#   --ref TAG      a release of sift built once under $EXTRAS_SRC (steps/builds.sh), instead of --build and --root
#   --docs         also write bench/results/scaling.tsv, redraw docs/img and rewrite the generated blocks
set -euo pipefail
export LC_ALL=C
HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=$(dirname "$HERE")
REPO=$(dirname "$BENCH")
export SCRATCH_CPU=${SCRATCH_CPU:-400%}
. "$HERE/scratch.sh"
. "$BENCH/versions.env"
. "$HERE/builds.sh"

ORIG=("$@")
REF='' DOCS='' ROWS=1000000 REPS=7 PARTS=1,4,16,64,256,1024,4096 WORKERS=0,1,2,3 OUT='' KEEP=${KEEP:-}
while (( $# )); do
	case $1 in
		--docs) DOCS=1 ;;
		--build) SIFT_BUILD=$(cd "$2" && pwd); shift ;;
		--ref) REF=$2; shift ;;
		--root) export SIFT_ROOT=$2; shift ;;
		--rows) ROWS=$2; shift ;;
		--reps) REPS=$2; shift ;;
		--partitions) PARTS=$2; shift ;;
		--workers) WORKERS=$2; shift ;;
		--out) OUT=$2; shift ;;
		--keep) KEEP=1 ;;
		*) sed -n '2,8p' "$0"; exit 1 ;;
	esac
	shift
done
export KEEP
if [[ -n $REF ]]; then L=$(build_label "$REF"); build_prepare "$L"; SIFT_BUILD=$(build_dir "$L"); export SIFT_ROOT=$EXTRAS_CACHE/$L/root; fi
scratch_scope "${ORIG[@]}"

OUT=${OUT:-$BENCH/out/scaling-$(date -u +%Y%m%dT%H%M%S).tsv}
mkdir -p "$(dirname "$OUT")"
trap scratch_down EXIT
scratch_up scaling
echo "max_locks_per_transaction = 512" >>"$SIFT_TMP/data/postgresql.conf"   # one CREATE INDEX on 4096 partitions locks every partition and index
"$PG" stop >&2; "$PG" start >&2
sq -d postgres -c "DROP DATABASE IF EXISTS scaling" -c "CREATE DATABASE scaling" >/dev/null
sq -d scaling -c "CREATE EXTENSION pg_sift" >/dev/null
echo "scaling: $(sq -d scaling -c 'SELECT sift.version()') (build $(git -C "$SIFT_BUILD" rev-parse --short HEAD)) on port $SIFT_PORT, $ROWS orders" >&2
sq -d scaling -v n="$ROWS" -f "$BENCH/sql/store/gen.sql" >/dev/null
meta=(--meta "date=$(date -u +%Y-%m-%d)" --meta "cpu=$(lscpu | awk -F: '/Model name/ {gsub(/^ +/, "", $2); n=$2} /^CPU\(s\)/ {gsub(/ /, "", $2); t=$2} END {printf "%s (%s threads)", n, t}'), scope ${SCRATCH_CPU} ${SCRATCH_MEM:-12G}"
	--meta "postgresql=PostgreSQL $(sq -d scaling -c 'SHOW server_version')" --meta "build=$(git -C "$SIFT_BUILD" describe --tags --always)" --meta "shared_buffers=$(sq -d scaling -c 'SHOW shared_buffers')"
	--meta "host_load_before=$(host_load)")
python3 -I "$HERE/scaling.py" run --db scaling --out "$OUT" --rows "$ROWS" --reps "$REPS" --partitions "$PARTS" --workers "$WORKERS" "${meta[@]}"
echo "scaling: $OUT" >&2
if [[ -n $DOCS ]]; then
	cp "$OUT" "$REPO/bench/results/scaling.tsv"
fi
