#!/usr/bin/env bash
# The WAL a sift_table writes where a reader would not look. The records of VACUUM that make the rows of a vblock visible to everyone name the newest transaction whose rows they are (the horizon
# a standby ends its queries at), the writer of a column delta of a row of the vblock as well as the writers of its parts: the VACUUM folds the settled delta into a new part, whose commit logs the
# conflict of the writers it freezes, before the record that sets the bits of the visibility map. A parallel worker that scans a table with a row
# in a micro-slab reads the slab where it is, as a standby does, and a query that only reads writes no WAL (in a worker the manifest is the leader's to write).
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 5
node_init vw 0 "shared_preload_libraries = 'pg_sift'
autovacuum = off
sift.async_maintenance = off"
node_start vw || bail "start failed"
sql vw "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_walinspect" >/dev/null || bail "extension failed"
F2=$([[ $(sql vw "SELECT count(*) FROM pg_settings WHERE name = 'sift.test_create_format1'") == 1 ]] && echo "SET sift.test_create_format1 = off" || echo "SELECT 1")
sql vw "$F2" \
	"CREATE TABLE t (id int, k int, v int, a int, b int, c int, d int, e int, f int, pad text) USING sift_table" \
	"INSERT INTO t SELECT g, g, g, g, g, g, g, g, g, md5(g::text) FROM generate_series(1, 20000) g" "ANALYZE t" "VACUUM t" >/dev/null || bail "setup failed"

lsn() { sql vw "SELECT pg_current_wal_insert_lsn()"; }
from=$(lsn)
xid=$(sql vw "BEGIN" "UPDATE t SET v = -1 WHERE id = 10" "SELECT pg_current_xact_id()" "COMMIT" | tail -1)
sql vw "VACUUM (TRUNCATE false) t" >/dev/null || bail "vacuum failed"
wait_for "wal_flushed vw" 30
records() { sql vw "SELECT record_type, coalesce((regexp_match(description, '(?:horizon |snapshotConflictHorizon: [0-9]+:)([0-9]+)'))[1], '-') FROM pg_get_wal_records_info('$from', pg_current_wal_insert_lsn())
	WHERE (resource_manager, record_type) IN (('pg_sift_table_vm', 'VM_SET'), ('Btree', 'REUSE_PAGE')) AND description LIKE '%/' || pg_relation_filenode('t') || '%' ORDER BY start_lsn"; }
seen=$(records)
horizon=$(awk -F'|' '$2 != "-" && $2 + 0 > h { h = $2 + 0 } $1 == "VM_SET" { exit } END { print h + 0 }' <<<"$seen")
out="the writer of the column delta is transaction $xid, the records of the VACUUM: $(tr '\n' ' ' <<<"$seen")"
check "VACUUM sets the bits again over the row with the column delta" grep -q '^VM_SET|' <<<"$seen"
check "the standby ends the queries older than the writer of the column delta before the bits are set" test "$horizon" -ge "$xid"

sql vw "$F2" "CREATE TABLE pt (id int, g int, v int) USING sift_table" "INSERT INTO pt VALUES (1, 1, 1), (2, 1, 2)" >/dev/null || bail "setup of pt failed"
from=$(lsn)
out=$(sql vw "SET debug_parallel_query = on" "SET min_parallel_table_scan_size = 0" "EXPLAIN (costs off) SELECT g, sum(v) FROM pt GROUP BY g" "SELECT g || '|' || sum(v) FROM pt GROUP BY g" 2>&1)
check "the query runs in a parallel worker" grep -q "Gather" <<<"$out"
check "and answers the rows of the slab" test "$(tail -1 <<<"$out")" == "1|3"
written=$(sql vw "SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), '$from')")
out="the query wrote $written bytes of WAL"
check "a query that only reads writes no WAL" test "$written" -lt 1000
finish
