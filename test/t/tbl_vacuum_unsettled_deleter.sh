#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A VACUUM that stops on a deleter that is not settled (a snapshot older than the delete is open) rewrites nothing and keeps nothing: the output part it wrote before it found the deleter
# is given back at once, so the file does not grow with each VACUUM that stops, and the VACUUM that follows the end of the snapshot settles every part and leaves the rows as they were.
. "$(dirname "$0")/lib.bash"

done_testing
node_init uw 2 "autovacuum = off
sift.async_maintenance = off
sift.test_create_format1 = off"
node_start uw || bail "start failed"
sql uw "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"

# two parts of 6,000 rows (one batch each) that a 1 MB budget compacts in one group; the rows of the first make more than the compaction holds, so it is written out before the second is read
sql uw "CREATE TABLE u (id bigint, k int, payload text) USING sift_table" \
	"INSERT INTO u SELECT g, g % 8, repeat('x', 200) FROM generate_series(1, 6000) g" \
	"INSERT INTO u SELECT g, g % 8, repeat('x', 200) FROM generate_series(6001, 12000) g" >/dev/null || bail "load failed"
size() { sql uw "SELECT pg_relation_size('u')"; }
digest() { sql uw "SELECT count(*) || '/' || sum(id) FROM u"; }
vacuum() { sql uw "SET maintenance_work_mem = '1MB'" "VACUUM u" >/dev/null; }

sql uw "DELETE FROM u WHERE id <= 1000" >/dev/null || bail "first delete failed"
coproc SNAP { psql -X -At -q -v ON_ERROR_STOP=1 -h "$(node_sock uw)" -p "$(port uw)" -U postgres -d "$(node_db uw)"; }
printf '%s\n' "BEGIN ISOLATION LEVEL REPEATABLE READ;" "SELECT count(*) AS c FROM u \\gset" "\\echo READY" >&"${SNAP[1]}"
read -r -u "${SNAP[0]}" ready
sql uw "DELETE FROM u WHERE id > 6000 AND id <= 7000" >/dev/null || bail "second delete failed"
want=$(digest)

check "the VACUUM under the open snapshot ends" vacuum
size1=$(size)
check "the VACUUM under the open snapshot again ends" vacuum
size2=$(size)
diag "bytes of the table after the first and the second VACUUM under the snapshot: $size1 $size2"
check "the second VACUUM does not grow the table" test "$size2" -le "$size1"
check "the rows are as they were" test "$(digest)" == "$want"

echo '\q' >&"${SNAP[1]}"
wait "$SNAP_PID"
check "the VACUUM after the snapshot ends" vacuum
check "the rows are as they were after it" test "$(digest)" == "$want"
check "no dead rows are left" test "$(sql uw "SELECT sum(dead_rows) FROM sift.parts('u')")" == 0
out=$(sql uw "SELECT problem || ' ' || coalesce(detail, '') FROM sift.check('u', true)")
check "sift.check finds nothing" test -z "$out"

finish
