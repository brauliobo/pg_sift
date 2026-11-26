#!/usr/bin/env bash
# tap: pooled
# The reads of an index never ask for a rewrite (sift.async_maintenance). Rows deleted from one segment leave its units dirty after the VACUUM, and a dirty unit is not served from the
# point directory: every read of a key in it reads the unit (60 keys: 0.90 M instructions before the VACUUM, 1.75 M after it). However many reads pay for it, no worker rewrites
# the index: the units stay dirty, the answers are the table's, sift.col_maintenance_status recommends REINDEX, and the REINDEX is what makes the reads cheap again.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 5
# Format 1: dirty units of a segment, the point directory and sift.col_maintenance_status; Format 2 has parts and deletion vectors
node_init rd 25 "sift.test_create_format1 = on
autovacuum = off
sift.maintenance_naptime = 200
sift.log_min_duration = 0"
node_start rd || bail "start failed"
D=$(dir rd)
sql rd "CREATE EXTENSION pg_sift" "CREATE TABLE t (id int, k int, v int, pad text)" \
	"INSERT INTO t SELECT g, g % 3000, g % 1000, md5(g::text) FROM generate_series(1, 300000) g" \
	"CREATE INDEX t_c ON t USING sift_col (id, k, v)" "VACUUM (ANALYZE, FREEZE) t" >/dev/null || bail "setup failed"
sql rd "ALTER DATABASE postgres SET sift.async_maintenance = on" >/dev/null

dirty() { sql rd "SELECT coalesce(sum(units) FILTER (WHERE dirty), 0) FROM sift.col_segments('t_c')"; }
ids=$(seq 1 333 20000 | head -60 | paste -sd, -)
# $1 batches of 100 reads of 60 keys in the dirty units in one session, a naptime apart
reads() { sql rd "SET enable_seqscan = off" "SET enable_bitmapscan = off" "SET sift.enable_agg = off" "SET max_parallel_workers_per_gather = 0" \
	"DO \$\$ BEGIN FOR b IN 1..$1 LOOP FOR i IN 1..100 LOOP PERFORM id, k, v FROM t WHERE id IN ($ids); END LOOP; PERFORM pg_sleep(0.3); END LOOP; END \$\$" >/dev/null; }
unchanged() { [[ $(grep -c 'rewrote (' "$D/postgres.log") == 0 && $(dirty) -gt 0 ]]; }
rows() { sql rd "SET sift.enable_agg = $1" "SELECT sum(v), count(*) FROM t WHERE id IN ($ids)" | tail -1; }

sql rd "DELETE FROM t WHERE id BETWEEN 1 AND 20000 AND id % 5 = 0" "VACUUM t" >/dev/null
check "the VACUUM left units with dead rows" test "$(dirty)" -gt 0
reads 40
check "the reads that paid for them started no rewrite, and the units are as they were" unchanged
check "the status recommends REINDEX" test "$(sql rd "SELECT count(*) FROM sift.col_maintenance_status('t_c') m, unnest(m.blocked_by) b WHERE b LIKE 'REINDEX recommended (dirty units)%'")" == 1
check "the answers are the table's" test "$(rows on)" == "$(sql rd "SET sift.enable_agg = off" "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SET enable_indexonlyscan = off" "SELECT sum(v), count(*) FROM t WHERE id IN ($ids)" | tail -1)"
sql rd "REINDEX INDEX CONCURRENTLY t_c" >/dev/null
check "after the REINDEX no unit is dirty and nothing is recommended" test "$(dirty)|$(sql rd "SELECT count(*) FROM sift.col_maintenance_status('t_c') m, unnest(m.blocked_by) b WHERE b LIKE 'REINDEX recommended%'")" == "0|0"
finish
