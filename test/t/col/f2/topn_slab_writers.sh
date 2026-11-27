#!/usr/bin/env bash
# A Top-N read of a sift_col index beside single-row writers (S114-22): concurrent inserts grow the micro-slab of a partition past the rows of a granule, and the read of that slab
# judges every one of its rows; the reader neither crashes nor errors, and its answer after the writers is the heap's.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 2
node_init p 3 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.test_create_format1 = off
max_parallel_workers_per_gather = 0"
node_start p || bail "start failed"
sql p "CREATE EXTENSION pg_sift" "CREATE TABLE tn (id int8, d1 int, v int8)" \
	"INSERT INTO tn SELECT g, g % 50, (g * 7919) % 100003 FROM generate_series(1, 50000) g" "CREATE INDEX tn_ix ON tn USING sift_col (id, d1, v)" "VACUUM tn" >/dev/null || bail "setup"
D=$(dir p)
TOPN="SELECT id, v FROM tn ORDER BY v DESC, id LIMIT 10"
echo "INSERT INTO tn VALUES (50000 + (random() * 1e6)::int, 1, (random() * 200000)::int);" >"$D/w.sql"
echo "$TOPN;" >"$D/r.sql"
pgbench_bg p 3 5 -f "$D/w.sql"
reader() { out=$(pgbench -n -h "$(node_sock p)" -p "$(port p)" -U postgres -c 1 -T 5 -f "$D/r.sql" "$(node_db p)" 2>&1) && wait "$PGBENCH_PID"; }
check "a Top-N read beside 3 single-row writers neither crashes nor errors" reader
heap=$(sql p "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SET sift.enable_agg = off" "$TOPN")
out=$(sql p "$TOPN")
check "and its answer after the writers is the heap's" test "$out" == "$heap"
finish
