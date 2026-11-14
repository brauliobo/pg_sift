#!/usr/bin/env bash
# An index that CREATE INDEX CONCURRENTLY (or REINDEX CONCURRENTLY) is building is in the catalog of the primary and, replayed, of a hot standby before its file holds a
# metapage: the build writes the data first and the metapage last, and the index is ready only when it ends (a failed build leaves it not ready for good). sift.workers and
# sift.col_index_status read the metapage of every sift index, so such an index made both fail for the whole database ("could not read blocks", "invalid page layout"), and the
# functions of an index answered with a corruption error. The views list the indexes that are ready, the functions say that the index is not ready to be read. The build is held
# before it starts by a writer of the table that stays open, and the standby is paused before the build is replayed.
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_BUILDING_ROWS 20000 5000)
plan 10
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 256MB
autovacuum = off"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" "CREATE TABLE t (id int, tenant_id int, amount int) WITH (autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, g % 50, g % 97 FROM generate_series(1, $ROWS) g" \
	"CREATE INDEX a_idx ON t USING sift_col (id, tenant_id, amount)" "VACUUM ANALYZE t" "CHECKPOINT" >/dev/null || bail "setup failed"
node_clone primary standby 3 "hot_standby_feedback = off
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
D=$(dir primary)
rm -f "$D/locked" "$D/release"

# a writer of the table that stays open until $D/release exists: CREATE INDEX CONCURRENTLY waits for it after it entered the index in the catalog
{
	echo "BEGIN;"
	echo "INSERT INTO t VALUES (-1, 1, 1);"
	echo "\\! touch '$D/locked'"
	echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"
	echo "COMMIT;"
} | sql primary >/dev/null &
WRITER=$!
until [[ -e $D/locked ]]; do sleep 0.05; done
sql primary "CREATE INDEX CONCURRENTLY b_idx ON t USING sift_col (id, tenant_id, amount)" >/dev/null &
BUILDER=$!
building() { sql "$1" "SELECT count(*) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE c.relname = 'b_idx' AND NOT i.indisready"; }
for _ in $(seq 300); do [[ $(building primary) == 1 ]] && break; sleep 0.1; done
check "the primary has b_idx in the catalog, not ready" test "$(building primary)" == 1
catchup primary standby || bail "the standby did not catch up"
check "the standby replayed it so" test "$(building standby)" == 1

for node in primary standby; do
	check "$node: sift.col_index_status lists the ready index only" test "$(sql $node "SELECT string_agg(index::text, ',') FROM sift.col_index_status")" == a_idx
	check "$node: sift.workers lists the ready index only" test "$(sql $node "SELECT string_agg(index::text, ',') FROM sift.workers")" == a_idx
done
out=$(sql standby "SELECT count(*) FROM sift.col_segments('b_idx')" 2>&1)
check "standby: a function of the index says it is not ready to be read" grep -q 'sift index "b_idx" is not ready to be read' <<<"$out"
check "standby: the ready index is read as before" test "$(sql standby "SELECT count(*) > 0 FROM sift.col_segments('a_idx')")" == t

# the standby stops replaying, the build runs on the primary: the standby keeps the index not ready, and the views keep working
sql standby "SELECT pg_wal_replay_pause()" >/dev/null
touch "$D/release"
wait "$WRITER" "$BUILDER" || bail "the writer or the build failed"
check "standby paused before the build: the views list a_idx only" test "$(sql standby "SELECT string_agg(index::text, ',') FROM sift.col_index_status")" == a_idx
sql standby "SELECT pg_wal_replay_resume()" >/dev/null
catchup primary standby || bail "the standby did not catch up after the build"
check "standby after the build: both indexes are listed and read" test "$(sql standby "SELECT string_agg(index::text, ',' ORDER BY index::text) FROM sift.col_index_status")" == a_idx,b_idx
finish
