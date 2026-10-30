#!/usr/bin/env bash
# VACUUM gives the free end of a sift_col index back under AccessExclusiveLock. A standby replays that lock record first and waits for every query that
# holds the index (up to max_standby_streaming_delay, then cancels it): a b-tree is never cut, so a long query on a replica stalled the replay of the
# primary's VACUUM for as long as it ran. While a standby streams, VACUUM takes an ExclusiveLock, which the standby does not replay, and cuts only the blocks the horizon
# (held back by the feedback of the standby) says no snapshot can read: what a rewrite released after the standby's transaction began is not reusable, and the segment the
# rewrite wrote lies behind it, so the file stays as it is (col_online_cut.sh has the cut once the transaction ends).
# Here the standby holds a transaction on the index with max_standby_streaming_delay = -1 (a stalled replay never gives up) and the primary churns and VACUUMs.
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_CUT_ROWS 200000 100000)
plan 6
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 256MB
autovacuum = off"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" \
	"CREATE TABLE t (id int, key int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, g % 1000, g % 997, md5(g::text) FROM generate_series(1, $ROWS) g" \
	"CREATE INDEX t_idx ON t USING sift_col (id, key, amount)" "VACUUM t" >/dev/null || bail "setup failed"
size() { sql primary "SELECT pg_relation_size('t_idx')"; }
# a rewrite leaves the index twice its size in the file: the segments it replaced are the free end that VACUUM cuts
rewrite() { sql primary "SET sift.async_maintenance = off" "UPDATE t SET amount = amount + 1 WHERE id % 3 = 0" "SELECT sift.col_merge('t_idx', true) >= 0" >/dev/null || bail "rewrite failed"; }

rewrite
grown=$(size)
sql primary "SET sift.async_maintenance = off" "VACUUM t" >/dev/null || bail "vacuum failed"
check "no standby: VACUUM cuts the free end of the file" test "$(size)" -lt "$grown"

node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s
max_standby_streaming_delay = -1"
node_start standby || bail "standby start failed"
D=$(dir standby)
catchup primary standby || bail "the standby did not catch up"

# a transaction of the standby that holds the index until $D/release exists
rm -f "$D/locked" "$D/release"
{
	echo "BEGIN ISOLATION LEVEL REPEATABLE READ;"
	echo "SET enable_seqscan = off;"
	echo "SELECT count(*), sum(amount) FROM t WHERE key = 5;"
	echo "\\! touch '$D/locked'"
	echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"
	echo "COMMIT;"
} | sql standby >"$D/query.out" 2>&1 &
HOLDER=$!
until [[ -e $D/locked ]]; do sleep 0.05; done
# the feedback of the standby (wal_receiver_status_interval) tells the primary the transaction's snapshot: until it arrives the horizon does not hold the blocks
sleep 3

rewrite
grown=$(size)
sql primary "SET sift.async_maintenance = off" "VACUUM t" >/dev/null || bail "vacuum failed"
LSN=$(sql primary "SELECT pg_current_wal_insert_lsn()")
sleep 3
check "a standby streams: the replay of VACUUM does not wait for its long query" test "$(sql standby "SELECT pg_last_wal_replay_lsn() >= '$LSN'")" == t
check "a standby streams: VACUUM leaves the free end of the file (the snapshot of the standby may read it)" test "$(size)" -ge "$grown"
touch "$D/release"
wait "$HOLDER"
check "the long query is not cancelled" test -z "$(grep -i 'conflict with recovery' "$D/query.out")"
catchup primary standby
check "the standby counts as the primary" test "$(sql standby "SELECT count(*), sum(amount) FROM t")" == "$(sql primary "SELECT count(*), sum(amount) FROM t")"
check "the standby verifies the index" test -z "$(sql standby "SELECT sift.col_verify('t_idx', true)" 2>&1)"
finish
