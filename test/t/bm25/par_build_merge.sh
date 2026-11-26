#!/usr/bin/env bash
# A build with workers and a merge of segments scale with the processes that take part: a CREATE INDEX with 4 workers (maintenance_work_mem = 32MB) takes at most half of the time of the build alone (the best of several runs of each);
# a merge of segments of about 9 MB under maintenance_work_mem = 16MB is cut into ranges for the workers (a range is an eighth of the maintenance memory, 4 MB at most, so that the merges of
# 8 MB and up are shared); the pages of the merged segment are logged 16 to a record; the indexes verify.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 7
ROWS=$(knob SIFT_PBM_ROWS 200000 200000)

node_init pbm 3 "autovacuum = off
sift.async_maintenance = off
max_worker_processes = 16
max_parallel_workers = 8
shared_buffers = 256MB
max_wal_size = 4GB"
node_start pbm || bail "start failed"
sql pbm "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_walinspect" \
	"CREATE TABLE d (id int, body text) WITH (autovacuum_enabled = off, parallel_workers = 4)" \
	"INSERT INTO d SELECT i, (SELECT string_agg('w' || ((i::bigint * 7919 + j * 104729 + i * j) % 300007) || ' x' || ((i + j) % 997), ' ') FROM generate_series(1, 25) j) FROM generate_series(1, $ROWS) i" \
	"VACUUM (INDEX_CLEANUP off, TRUNCATE off) d" >/dev/null || bail "setup failed"

ms() { local t0=$(date +%s%N); "$@" >/dev/null || bail "$* failed"; echo $((($(date +%s%N) - t0) / 1000000)); }
build() {
	sql pbm "DROP INDEX IF EXISTS d_i" "SET maintenance_work_mem = '32MB'" "SET max_parallel_maintenance_workers = $1" "CREATE INDEX d_i ON d USING sift (body)"
}
best() { local w=$1 n=$2 t b=; for _ in $(seq "$n"); do t=$(ms build "$w"); [[ -n $b && $b -le $t ]] || b=$t; done; echo "$b"; }
ms_serial=$(best 0 2)
ms_par=$(best 4 3)
diag "build of $ROWS docs, the best of several: ${ms_serial} ms alone, ${ms_par} ms with 4 workers"
out="alone $ms_serial ms, 4 workers $ms_par ms"
check "a build with 4 workers takes at most half of the time of the build alone" test $((ms_par * 2)) -le "$ms_serial"
check "the index of the build with workers verifies" test "$(sql pbm "SELECT sift.verify('d_i', true)")" == ""

sql pbm "DROP INDEX d_i" "CREATE TABLE m AS SELECT * FROM d WHERE id <= $((ROWS * 16 / 100))" \
	"CREATE INDEX d_m ON m USING sift (body) WITH (build_segments = 4, merge_fanin = 4)" >/dev/null || bail "index of the merge failed"
SEGS=$(sql pbm "SELECT count(*) FROM sift.segments('d_m')")
BYTES=$(sql pbm "SELECT sum(bytes) FROM sift.segments('d_m')")
diag "segments to merge: $SEGS, $BYTES bytes"
read -r pieces big_records big_pages pages merged < <(sql pbm <<EOF | tr '|' ' '
SET maintenance_work_mem = '16MB';
SET max_parallel_maintenance_workers = 4;
SET sift.test_range_pieces = 0;
SELECT pg_current_wal_insert_lsn() AS from_lsn \gset
SELECT sift.merge('d_m', true) AS merged \gset
SELECT current_setting('sift.test_range_pieces')::int AS pieces \gset
SELECT pg_current_wal_insert_lsn() AS to_lsn \gset
SELECT :pieces, count(*) FILTER (WHERE n >= 8), coalesce(sum(n) FILTER (WHERE n >= 8), 0),
	(SELECT (sum(bytes) / 8192)::int FROM sift.segments('d_m') WHERE kind = 'immutable'), :merged
FROM (SELECT count(*) n FROM pg_get_wal_block_info(:'from_lsn', :'to_lsn') GROUP BY start_lsn) b;
EOF
)
diag "merge: $merged merged, $pieces ranges, $big_pages of $pages pages in $big_records records of 8 pages or more"
out="ranges $pieces, pages in records of 8 or more $big_pages of $pages in $big_records records"
check "the segments were merged" test "$merged" -ge 1
check "a merge of segments under maintenance_work_mem = 16MB is cut into ranges for the workers" test "$pieces" -gt 1
check "at least 80% of the pages of the merged segment are logged 8 or more to a record" test $((big_pages * 5)) -ge $((pages * 4))
check "the merged index verifies" test "$(sql pbm "SELECT sift.verify('d_m', true)")" == ""
check "the merge left one segment" test "$(sql pbm "SELECT count(*) FROM sift.segments('d_m') WHERE kind = 'immutable'")" -eq 1
finish
