#!/usr/bin/env bash
# The merge of a parallel build by the participants (col_build_slices.sh) next to a streaming standby: a slice that PANICs after it wrote its segment and before the
# directory switch takes the primary down, the build never committed, and neither the primary nor the standby has the index or any block of it; the build that follows
# is replayed whole, the standby verifies it and answers as the heap does, and the file is the live bytes on both.
. "$(dirname "$0")/lib.bash"

plan 7
node_init primary 24 "wal_level = replica
max_wal_senders = 4
autovacuum = off
max_worker_processes = 16
max_parallel_workers = 8"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE t (id int, dim_b_id int, day date, v int, pad text) WITH (fillfactor = 20, autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, 1 + (g::bigint * 7919 % 12)::int, date '2018-01-01' + (((g::bigint * 2654435761) % 4294967296 / 65536) % 2900)::int, g % 100, repeat('x', 60) FROM generate_series(1, 240000) g" \
	"VACUUM (FREEZE, ANALYZE) t" "ALTER TABLE t SET (parallel_workers = 4)" >/dev/null || bail "setup failed"
node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"
OPTS="SET max_parallel_maintenance_workers = 4"
SMALL="SET min_parallel_table_scan_size = 0"
PART="SET sift.col_auto_merge_partition_kb = 256"
SEG="SET sift.test_col_segment_kb = 64"
CREATE="CREATE INDEX t_idx ON t USING sift_col (dim_b_id, day, v) WITH (key_cut = 'dim_b_id, day')"
has_index() { sql "$1" "SELECT count(*) FROM pg_class WHERE relname = 't_idx'"; }
answer() { sql "$1" "SET enable_seqscan = off" "SELECT count(*), sum(v) FROM t WHERE dim_b_id = 7"; }
heap() { sql primary "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SELECT count(*), sum(v) FROM t WHERE dim_b_id = 7"; }

SIFT_TMP=$(dir primary) leak_expect 1 'PANIC:'
SIFT_TMP=$(dir primary) leak_expect 1 'terminated by signal 6'
panics() { ! sql primary "SET sift.test_col_merge_panic = on" "$OPTS" "$SMALL" "$PART" "$SEG" "$CREATE" >/dev/null 2>&1; }
check "a slice of the merge PANICs after it wrote its segment" panics
for _ in $(seq 300); do [[ -f $(dir primary)/data/postmaster.pid ]] || break; sleep 0.1; done
check "the primary restarts through crash recovery" node_start primary
catchup primary standby
check "the primary has no index of the build that stopped" test "$(has_index primary)" == 0
check "the standby has none either" test "$(has_index standby)" == 0
sql primary "$OPTS" "$SMALL" "$PART" "$SEG" "$CREATE" >/dev/null || bail "the build after the crash failed"
catchup primary standby
check "the standby verifies the index of the build that followed" test -z "$(sql standby "SELECT sift.col_verify('t_idx', true)" 2>&1)"
check "... and answers as the heap does" test "$(answer standby)" == "$(heap)"
file_pct() { sql "$1" "SELECT 100 * pg_relation_size('t_idx') / bytes FROM sift.col_index_info('t_idx')"; }
check "... with the file the live bytes on both (at most 110%)" test "$(file_pct primary)" -le 110 -a "$(file_pct standby)" -le 110
finish
