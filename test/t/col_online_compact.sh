#!/usr/bin/env bash
# While a physical standby streams, VACUUM cannot move a segment under the lock of the space step (the standby would replay the lock and wait for its queries). The
# segments that end the file are written again into the reusable holes below them by the ordinary rewrite instead (sift_maint_compact), and the online cut takes the blocks
# they leave once no snapshot can read them: an index rewritten whole, whose one segment lies behind the blocks of the five it replaced (3.8 times the live bytes), comes back
# to its live bytes, the replay does not stall behind a long query of the standby on the table, and the index agrees with the table on both servers.
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_OC_ROWS 400000 200000)
plan 8
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 512MB
autovacuum = off
max_worker_processes = 16
max_parallel_workers = 8"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE t (id int, grp int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, g % 500, g % 997, md5(g::text) || md5((g * 7)::text) FROM generate_series(1, $ROWS) g" \
	"VACUUM (FREEZE, ANALYZE) t" >/dev/null || bail "setup failed"
node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s
max_standby_streaming_delay = -1"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"
D=$(dir primary)
size() { sql primary "SELECT pg_relation_size('t_idx')"; }
live() { sql primary "SELECT bytes FROM sift.col_index_info('t_idx')"; }
ratio() { echo "# file $(size) live $(live)" >&2; echo $(( 100 * $(size) / $(live) )); }
vacuum() { sql primary "SET sift.async_maintenance = off" "VACUUM t" >/dev/null; }
past() { sql primary "SELECT pg_current_xact_id()" >/dev/null; catchup primary standby; sleep 3; }
# a layout with a hole by construction: five partitions of the key range, the first two emptied by a DELETE and rewritten by the VACUUM: the live segments of the rewrite lie behind the
# blocks the emptied ones leave, which are free (reusable once the horizon passes them) in front of the live segments
layout() { sql primary "SELECT 'seg', gen, kind, bytes/8192 AS blocks FROM sift.col_segments('t_idx') ORDER BY gen" "SELECT 'free', start, nblocks, start+nblocks, pending FROM sift._test_freelist('t_idx') ORDER BY start" "SELECT 'info', relation_blocks, high_water, free_blocks, reusable_blocks FROM sift.col_index_info('t_idx')" | sed 's/^/# LAYOUT /' >&2; }
sql primary "SET max_parallel_maintenance_workers = 4" "SET min_parallel_table_scan_size = 0" "SET sift.col_auto_merge_partition_kb = 2048" \
	"CREATE INDEX t_idx ON t USING sift_col (id, grp, amount, pad) WITH (key_cut = grp)" >/dev/null || bail "create failed"
catchup primary standby
sql primary "ALTER INDEX t_idx SET (dead_rewrite_pct = 20)" "DELETE FROM t WHERE grp < 200" >/dev/null
past
vacuum
sql primary "SET sift.async_maintenance = off" "SELECT sift.col_maintain('t_idx')" | sed 's/^/# MAINTAIN /' >&2
past
echo "# built: $(ratio)% of the live bytes" >&2
check "a rewrite of the whole index leaves its one segment behind the blocks of the five it replaced (the file is over 2 times the live bytes)" test "$(ratio)" -gt 200
check "... and those blocks are all reusable" test "$(sql primary "SELECT reusable_blocks > 0 AND reusable_blocks = free_blocks FROM sift.col_index_info('t_idx')")" == t

# a standby query on the table holds the horizon back: the blocks the rewrite releases stay in the file until it ends, and the replay does not wait for it
rm -f "$D/held"
{ echo "BEGIN ISOLATION LEVEL REPEATABLE READ;"; echo "SELECT count(*) FROM t WHERE id < 10;"; echo "\\! touch '$D/held'"; echo "SELECT pg_sleep(15);"
  echo "SET enable_seqscan = off; SELECT 'after', count(*), sum(amount) FROM t WHERE grp = 5;"; echo "COMMIT;"; } | sql standby >"$D/hold.out" 2>&1 &
HOLDER=$!
until [[ -e $D/held ]]; do sleep 0.05; done
wal0=$(sql primary "SELECT pg_current_wal_insert_lsn()")
t0=$(date +%s.%N)
sql primary "SET sift.async_maintenance = off" "SET client_min_messages = log" "VACUUM (VERBOSE) t" 2>&1 | grep -E "rewrote|truncated" | sed 's/^/# VAC /' >&2
echo "# first VACUUM: $(echo "$(date +%s.%N) - $t0" | bc) s, WAL $(sql primary "SELECT pg_size_pretty(pg_wal_lsn_diff(pg_current_wal_insert_lsn(), '$wal0'))")" >&2
lsn=$(sql primary "SELECT pg_current_wal_insert_lsn()")
sleep 3
check "the segments are written again into the holes while the query holds the table: the replay does not stall behind it" test "$(sql standby "SELECT pg_last_wal_replay_lsn() >= '$lsn'")" == t
wait "$HOLDER"
check "... and the query reads what the primary reads" test "$(grep after "$D/hold.out" | tr -d ' ')" == "after|$(sql primary "SET enable_seqscan = off" "SELECT count(*), sum(amount) FROM t WHERE grp = 5")"

# the query is over: the next VACUUMs cut what the rewrite left (the feedback of the standby reaches the primary a second after a snapshot ends)
for _ in $(seq 15); do
	past
	vacuum
	[[ $(ratio) -le 115 ]] && break
done
check "once the snapshot ends the file is back to its live bytes (at most 1.15 times)" test "$(ratio)" -le 115
check "the index counts as the table" test "$(sql primary "SET enable_seqscan = off" "SELECT count(*), sum(amount) FROM t WHERE grp = 5")" == "$(sql primary "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SET sift.enable_col_scan = off" "SELECT count(*), sum(amount) FROM t WHERE grp = 5")"
catchup primary standby
check "the standby verifies the index" test -z "$(sql standby "SELECT sift.col_verify('t_idx', true)" 2>&1)"
check "... and answers as the primary" test "$(sql standby "SET enable_seqscan = off" "SELECT count(*), sum(amount) FROM t WHERE grp = 5")" == "$(sql primary "SET enable_seqscan = off" "SELECT count(*), sum(amount) FROM t WHERE grp = 5")"
finish
