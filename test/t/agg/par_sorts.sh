#!/usr/bin/env bash
# The bounded-memory gate of the external sorts, the parallel aggregate and the scans that spill (scripts/par-sorts-gate.py, test/scaling/par_sorts.tsv): every case at 1x, 2x and 4x rows in a fresh
# backend with a small work_mem. A case passes when its peak memory stays within 1.15 of the one at 1x, its growth is at most 2 bytes a row of the memory the scopes of sift count and 8 bytes a row
# of the resident peak, its peak above the start of the backend is at most 2.5 times the hash memory and 4 MB, the temporary files it holds at once are at most 2.5 times what it spills and
# a few files, and none is left (or leaks a resource) after it.
# A server built with assertions is skipped: it checks every allocation, so its resident peaks are not those of a release server.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
REPO=${SIFT_REPO:?}

[[ -r /proc/self/status ]] || { echo '1..0 # SKIP needs /proc'; exit 0; }
node_init ps 34 "autovacuum = off
max_parallel_workers_per_gather = 0
shared_buffers = 64MB
max_wal_size = 1GB
sift.async_maintenance = off"
node_start ps || bail "start failed"
if [[ $(sql ps "SHOW debug_assertions") == on ]]; then echo '1..0 # SKIP an assert build checks every allocation: its resident peaks are not those of a release server'; exit 0; fi
sql ps "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extension failed"

gate() { PGHOST=$(dir ps)/sock PGPORT=$(port ps) PGUSER=postgres PGDATABASE=postgres python3 -I "$REPO/scripts/par-sorts-gate.py" "$@"; }
out=$(gate run --only "${PAR_SORTS_ONLY:-.}" 2>"$(dir ps)/gate.err") || true
lines=$(grep '^CASE ' <<<"$out" || true)
n=$(grep -c . <<<"$lines")
plan $((n + 1))
check "every case ran" test "$n" -eq "$(gate list --only "${PAR_SORTS_ONLY:-.}" | grep -c .)"
while IFS= read -r l; do
	[[ -n $l ]] || continue
	diag "$l"
	if [[ $l == *" PASS" ]]; then ok "${l#CASE }"; else not_ok "${l#CASE }"; fi
done <<<"$lines"
finish
