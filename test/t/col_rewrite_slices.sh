#!/usr/bin/env bash
# The merge of segments outside a build (the rewrite a VACUUM or sift.col_maintain starts, sift.col_merge) by the backend and max_parallel_maintenance_workers
# workers, each a slice of the key range (col/maint/merge.c, merge_by_workers): from an input of 12 MB (here lowered to 2 MB), 3 slices. The backend has no
# transaction id there, so the workers take no lock that needs one. What a query sees is what the merge of one process leaves: the rows, the answers and
# nothing left to rewrite (the 3 segments are disjoint in the key: a rewrite does not take them again); a smaller input, or no worker, is merged by one process.
# An output cut into partitions (sift.col_auto_merge_partition_kb) is a slice for each partition from 2, the workers no more than the slices less the backend.
. "$(dirname "$0")/lib.bash"

plan 16
node_init mc 22 "autovacuum = off
max_worker_processes = 16
max_parallel_workers = 8"
node_start mc || bail "start failed"
sql mc "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE t (id int, dim_b_id int, day date, v int, w int)" \
	"INSERT INTO t SELECT g, 1 + (g::bigint * 7919 % 12)::int, date '2018-01-01' + (((g::bigint * 2654435761) % 4294967296 / 65536) % 2900)::int, g % 100, (g::bigint * 40503 % 1000003)::int FROM generate_series(1, 400000) g" \
	"VACUUM (FREEZE, ANALYZE) t" >/dev/null || bail "setup failed"
SEG="SET sift.test_col_segment_kb = 64"
CREATE="CREATE INDEX t_idx ON t USING sift_col (dim_b_id, day, v, w) WITH (key_cut = 'dim_b_id, day')"
QUERY="SELECT dim_b_id, count(*), sum(v), sum(w) FROM t WHERE day >= date '2022-03-01' AND day < date '2022-04-01' GROUP BY dim_b_id ORDER BY 1"
# a fresh index of several segments (a build without workers), merged with $1 workers; the slices the merge was made in
slices_of() { sql mc "DROP INDEX IF EXISTS t_idx" "SET max_parallel_maintenance_workers = 0" "$SEG" "$CREATE" "SET sift.test_col_merge_par_kb = ${2:-2048}" "SET max_parallel_maintenance_workers = $1" "SELECT sift.col_merge('t_idx', true)" "SHOW sift.test_col_merge_slices" | tail -n 1; }
# the same, the output cut into partitions of $2 kB (1.5 MB of input: 2 slices; 3 MB: 4)
cut_slices_of() { sql mc "DROP INDEX IF EXISTS t_idx" "SET max_parallel_maintenance_workers = 0" "$SEG" "$CREATE" "SET sift.col_auto_merge_partition_kb = $2" "SET max_parallel_maintenance_workers = $1" "SELECT sift.col_merge('t_idx', true)" "SHOW sift.test_col_merge_slices" | tail -n 1; }
shape() { sql mc "SELECT i.segments, i.rows FROM sift.col_index_info('t_idx') i" | tr '|' ' '; }
answers() { sql mc "SET enable_seqscan = off" "SET max_parallel_workers_per_gather = 0" "$QUERY" | "$REPO/scripts/sha256.sh"; }

check "the index is over 2 MB of segments" test "$(sql mc "SET max_parallel_maintenance_workers = 0" "$SEG" "$CREATE" "SELECT bytes / 1048576 >= 2 FROM sift.col_index_info('t_idx')" | tail -n 1)" == t
check "a merge outside a build is made in slices by the backend and its workers (a slice for each)" test "$(slices_of 2)" == 3
read -r PSEG PROWS <<<"$(shape)"
PANS=$(answers)
check "the index verifies" test -z "$(sql mc "SELECT sift.col_verify('t_idx', true)" 2>&1)"
check "nothing is left to rewrite" test "$(sql mc "SELECT coalesce(sift.col_maintenance_due('t_idx', false), 'none')")" == none
check "the output is segments (a slice holds no row when a unit is larger than its share: the units of a key are not split)" test "$PSEG" -ge 2
check "with no worker one process merges" test "$(slices_of 0)" == 0
read -r _ OROWS <<<"$(shape)"
OANS=$(answers)
check "the same rows" test "$PROWS" == "$OROWS"
check "the same answers" test "$PANS" == "$OANS"
check "four workers asked for, 3 slices (the backend and 2)" test "$(slices_of 4)" == 3
check "under the input the gate asks for (12 MB) one process merges" test "$(slices_of 2 12288)" == 0
check "a cut output is merged in slices by the backend and one worker" test "$(cut_slices_of 1 2048)" -ge 2
CUTROWS=$(shape | cut -d' ' -f2)
check "a cut output keeps the rows" test "$CUTROWS" == "$PROWS"
check "and the answers" test "$(answers)" == "$PANS"
check "with no worker one process merges a cut output" test "$(cut_slices_of 0 2048)" == 0
check "it answers as the heap does" test "$(sql mc "SET enable_seqscan = off" "SELECT count(*), sum(v) FROM t WHERE dim_b_id = 7")" == "$(sql mc "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SELECT count(*), sum(v) FROM t WHERE dim_b_id = 7")"
sql mc "DROP INDEX t_idx" "CREATE TABLE s AS SELECT * FROM t WHERE id <= 20000" "CREATE INDEX s_idx ON s USING sift_col (dim_b_id, day, v, w) WITH (key_cut = 'dim_b_id, day')" >/dev/null
check "a small index is merged by one process" test "$(sql mc "$SEG" "SET max_parallel_maintenance_workers = 2" "SELECT sift.col_merge('s_idx', true)" "SHOW sift.test_col_merge_slices" | tail -n 1)" == 0
finish
