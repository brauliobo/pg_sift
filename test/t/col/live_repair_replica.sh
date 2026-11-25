#!/usr/bin/env bash
# Physical streaming replica (pg_basebackup): sift.col_repair on the primary restores the liveness pages of a sift_col index that a VACUUM of 4.2.1 to 4.3.5 damaged in a
# segment of more than 65,152 superblocks (the special space of the first page overwritten, a dirty bit lost), and the restored pages replay to bytes equal to the primary's.
# The damage is made on the primary and written to disk by a checkpoint, so the base backup carries it to the standby.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 8
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = $(knob SIFT_WAL_KEEP 1GB 256MB)
autovacuum = off"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift_test CASCADE" "CREATE EXTENSION pageinspect" \
	"CREATE FUNCTION expect(q text) RETURNS text LANGUAGE plpgsql AS \$\$
	BEGIN
		EXECUTE q;
		RETURN 'ok';
	EXCEPTION WHEN OTHERS THEN
		RETURN SQLSTATE;
	END \$\$" \
	"CREATE TABLE r (id int, v int, pad text) WITH (autovacuum_enabled = false)" "ALTER TABLE r ALTER COLUMN pad SET STORAGE PLAIN" \
	"INSERT INTO r SELECT i, i % 10, repeat('x', 1000) FROM generate_series(1, 6000) i" \
	"CREATE INDEX r_idx ON r USING sift_col (id, v)" \
	"DELETE FROM r WHERE id % 7 = 0" "VACUUM (INDEX_CLEANUP ON) r" >/dev/null || bail "setup failed"
read -r LIVE NLIVE <<<"$(sql primary "SELECT first_block, nblocks FROM sift.col_extents('r_idx') WHERE what = 'liveness run'" | tr '|' ' ')"
HEAP=$(sql primary "SELECT count(*) FROM r")
ROWS="SELECT count(*) FROM sift._test_col_tids('r_idx', '', 2)"
# the page without its LSN and checksum: a standby sets the checksum of a page it replayed, a buffer of the primary has it only once it is written
PAGES="SELECT md5(string_agg(substr(get_raw_page('r_idx', b), 11), '' ORDER BY b)) FROM generate_series($LIVE, $LIVE + $NLIVE - 1) b"
sql primary "SELECT sift._test_poke('r_idx', $LIVE, 32, 8, 1)" "SELECT sift._test_poke('r_idx', $LIVE, 8178, 4, 2)" "CHECKPOINT" >/dev/null || bail "damage failed"

node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
ok "standby streaming from a base backup that holds the damaged pages"
check "standby: the verify finds the damage" test "$(sql standby "SELECT expect('SELECT sift.col_verify(''r_idx'')')")" == XX002
check "standby: the repair is refused during recovery" test "$(sql standby "SELECT expect('SELECT * FROM sift.col_repair(''r_idx'')')")" == 25006

check "primary: the repair restores the segment" test "$(sql primary "SELECT count(*) FROM sift.col_repair('r_idx')")" == 1
catchup primary standby || bail "standby does not catch up"
check "standby: the verify passes" test "$(sql standby "SELECT expect('SELECT sift.col_verify(''r_idx'')')")" == ok
check "standby: the liveness pages are the primary's bytes, but for their checksum" test "$(sql standby "$PAGES")" == "$(sql primary "$PAGES")"
check "standby: the index returns the rows of the heap and no dead one" test "$(sql standby "$ROWS")" == "$HEAP"
check "primary: a second repair finds nothing" test "$(sql primary "SELECT count(*) FROM sift.col_repair('r_idx')")" == 0
