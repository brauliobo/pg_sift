#!/usr/bin/env bash
# A long search with test/fuzz/diff_fuzz.py in short slices: each slice is a few seeds under `timeout` and `nice`, so that a shared host
# is never held for long; a divergence is shrunk and written to OUT (a self-contained reproducer each), the exit status is 1 when any slice
# found one. Needs the scripts/pgtest.sh cluster (SIFT_TMP, SIFT_PORT) installed and started.
#
#   test/fuzz/diff_fuzz_search.sh [--from N] [--to N] [--slice N] [--queries N] [--rows N] [--parts N] [--out DIR] [--lock FILE] [-- diff_fuzz.py options]
#
# Defaults: seeds 1000-1099 in slices of 10, 100 queries of 8000 rows each, 4 partitions, reproducers in ./diff-fuzz-repro, no lock.
# --lock FILE runs every slice under `flock -o` of FILE (the shared build lock of a busy host). --parts 4096 builds the table with 4096 partitions.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
FROM=1000 TO=1099 SLICE=10 QUERIES=100 ROWS=8000 PARTS=4 OUT=./diff-fuzz-repro LOCK= SECS=280
while (( $# )); do
	case $1 in
		--from) FROM=$2; shift ;; --to) TO=$2; shift ;; --slice) SLICE=$2; shift ;; --queries) QUERIES=$2; shift ;; --rows) ROWS=$2; shift ;;
		--parts) PARTS=$2; shift ;; --out) OUT=$2; shift ;; --lock) LOCK=$2; shift ;; --secs) SECS=$2; shift ;;
		--) shift; break ;;
		*) sed -n '2,12p' "$0"; exit 1 ;;
	esac
	shift
done

rc=0
for (( s = FROM; s <= TO; s += SLICE )); do
	e=$(( s + SLICE - 1 < TO ? s + SLICE - 1 : TO ))
	cmd=(nice timeout "$SECS" python3 -I "$HERE/diff_fuzz.py" --seeds "$s-$e" --queries "$QUERIES" --rows "$ROWS" --parts "$PARTS" --out "$OUT" "$@")
	[[ -z $LOCK ]] || cmd=(flock -o -w 3600 "$LOCK" "${cmd[@]}")
	"${cmd[@]}" || rc=1
done
exit $rc
