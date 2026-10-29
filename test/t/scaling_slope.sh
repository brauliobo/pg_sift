#!/usr/bin/env bash
# The scaling gate: the time of a request against the size of the table, at 5 sizes of small tables, with the log-log slope asserted per class: units pruned (every unit tested, none read), one unit read, count(*), sum, ORDER BY .. LIMIT, a point and an IN list through the point directory,
# GROUP BY with up to 256k groups, a request of 51 keys over 400-2000 partitions and over 1,000 partitions of padded rows (flat: the partitions it touches, not the partitions that
# exist) and the units a 51-key request reads in an arrival-order heap with an exact key summary (flat: bounded by the rows it asks for). Per-request work must stay flat (slope <= 0.5) and whole-table work linear (<= 1.15), so a change that makes a request cost more the more
# units, groups or partitions exist fails here, in seconds, instead of at 10x. The library is the 8-page-unit flavour of scripts/scaling-gate.sh (thousands of
# units on 1 GB of tmpfs); skipped on PostgreSQL 17.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

root=$("$REPO/scripts/scaling-gate.sh" build 2>/dev/null)
rc=$?
if ((rc == 77)); then echo '1..0 # SKIP the 8-page flavour needs PostgreSQL 18'; exit 0; fi
((rc == 0)) || { echo '1..1'; echo 'not ok 1 - build of the 8-page flavour'; exit 1; }

node_init sg 31 "wal_level = minimal
max_wal_senders = 0
autovacuum = off
shared_buffers = 128MB
max_locks_per_transaction = 512
max_wal_size = 256MB
sift.col_auto_merge = off
extension_control_path = '$root$(pg_config --sharedir):\$system'
dynamic_library_path = '$root$(pg_config --pkglibdir):\$libdir'"
node_start sg || bail "start failed"
sql sg "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"

out=$(PGHOST=$(dir sg)/sock PGPORT=$(port sg) PGUSER=postgres PGDATABASE=postgres SCALING_UNIT_PAGES=8 \
	python3 "$REPO/bench/scaling/scaling.py" run --gate --reps 5 2>"$(dir sg)/scaling.err")
echo "# $(grep -c '^SLOPE' <<<"$out") slope lines; the points are in $(dir sg)/scaling.err and the CSV below" >&2
grep -v '^SLOPE\|^CHECK' <<<"$out" | sed 's/^/# /' >&2
grep ': psql:' "$(dir sg)/scaling.err" | cut -c1-300 | sed 's/^/# /' >&2	# a case that failed says why in the log of the test

lines=$(grep '^SLOPE .* sift ' <<<"$out")
n=$(grep -c . <<<"$lines")
plan $((n + 5))
check "the answers of the scan, of the point directory, of an IN list and of the point keys of an aggregate are the heap's (4 checks)" test "$(grep -c "^CHECK .* ok" <<<"$out")" == 4
check "a slope that fails is measured again, and a growth every pass shows still fails (bench/scaling/test_retry.py)" python3 -B "$REPO/bench/scaling/test_retry.py"
check "no check mismatched" test "$(grep -c '^CHECK .*MISMATCH' <<<"$out")" == 0
check "no case failed to build or to measure" test "$(grep -c ',,,,,,,,ERROR' <<<"$out")" == 0
check "every class of the gate ran (at least 17 asserted slope lines)" test "$(grep -c " target=[a-z]* \(PASS\|FAIL\|NOSIFT\)" <<<"$lines")" -ge 17
while IFS= read -r l; do
	[[ -n $l ]] || continue
	desc=${l#SLOPE }
	if [[ $l == *" PASS "* || $l == *" INFO "* ]]; then ok "$desc"; else not_ok "$desc"; fi
done <<<"$lines"
