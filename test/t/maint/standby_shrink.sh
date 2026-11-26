#!/usr/bin/env bash
# A primary with a streaming standby gives the free tail of a sift_col index back to the file system, as a VACUUM of a heap does: the extents that a compaction released are cut once they
# are past the standby-safe horizon (sift.standby_conflicts = off: the horizon of the primary), the standby replays the lock and the truncation, and its queries read the index as the primary does.
# The VACUUMs that follow a delete of most rows compact the index at its end and quarantine what they replace; a later one finds the quarantine matured and cuts the free run behind the part.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 3
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 256MB
sift.test_create_format1 = off
sift.standby_conflicts = off
autovacuum = off"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" "CREATE TABLE h (id int, a bigint, b int)" "INSERT INTO h SELECT g, g, g % 100 FROM generate_series(1, 400000) g" \
	"CREATE INDEX h_col ON h USING sift_col (id, a, b)" >/dev/null || bail "setup failed"
node_clone primary standby 3 "hot_standby_feedback = off"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"
size() { sql primary "SELECT relation_blocks FROM sift.space('h_col')"; }
streams() { [[ $(sql primary "SELECT count(*) FROM pg_stat_replication WHERE state = 'streaming'") == 1 ]]; }
wait_for streams 30 || bail "the standby does not stream"

sql primary "DELETE FROM h WHERE id > 40000" >/dev/null
peak=0
for _ in $(seq 6); do
	sql primary "VACUUM h" >/dev/null
	((peak = $(size) > peak ? $(size) : peak))
done
after=$(size)
cut=$(sql primary "SELECT last_shrink IS NOT NULL FROM sift.space('h_col')")
check "the VACUUM of a primary with a streaming standby cuts the free tail of the index (at most $peak blocks, $after after the cut)" test "$cut|$((after < peak))" == "t|1"
catchup primary standby || bail "the standby did not catch up after the cut"
check "the standby reads the cut index as the primary does" test "$(sql standby "SET enable_seqscan = off" "SELECT count(*), sum(a) FROM h WHERE id > 0")" == "$(sql primary "SET enable_seqscan = off" "SELECT count(*), sum(a) FROM h WHERE id > 0")"
check "sift.check finds nothing in the index" test "$(sql primary "SELECT count(*) FROM sift.check('h_col', true)")" == 0
