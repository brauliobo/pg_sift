#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The first writer of a sift_table lays out the empty file it was given (the metapage and the directory, table/store.c). The pages must not reach the file before their WAL is durable:
# a page in the file whose WAL is lost leaves a block the standby and the recovery do not have, and the next record of it carries no full image (its LSN is past the redo pointer), so the
# standby stops with "WAL contains references to invalid pages" and the primary's next recovery fails. The first writer's transaction is open when the power goes (the walwriter does not run
# within the test), with a streaming standby under generic WAL consistency checking: the primary recovers, loads the table, crashes again with the load open, recovers again, and the standby
# replays the same rows.
. "$(dirname "$0")/lib.bash"

ROWS=10
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 32MB
autovacuum = off
wal_writer_delay = 10s
wal_writer_flush_after = 1GB
wal_consistency_checking = 'generic'"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" "CREATE TABLE lt (id int, grp int) USING sift_table" >/dev/null || bail "setup failed"
node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s
wal_consistency_checking = 'generic'"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"

LOAD="INSERT INTO lt SELECT g, g % 5 FROM generate_series(1, $ROWS) g"
# the power goes while the first writer's transaction is open: its WAL was never flushed
power_loss() { # LABEL
	sql primary "SELECT pg_switch_wal()" >/dev/null
	{ echo "BEGIN;"; echo "$LOAD;"; echo "SELECT pg_sleep(600);"; } | sql primary >/dev/null 2>&1 &
	local loader=$!
	for _ in $(seq 600); do [[ $(sql primary "SELECT pg_relation_size('lt') >= 16384") == t ]] && break; sleep 0.05; done
	check "$1: the first writer laid out the file" test "$(sql primary "SELECT pg_relation_size('lt') >= 16384")" == t
	crash primary
	wait "$loader" 2>/dev/null
	check "$1: the primary recovers" node_start primary
}
rows() { sql "$1" "SELECT count(*) FROM lt"; }

power_loss "power loss in the first write"
check "... the table is empty" test "$(rows primary)" == 0
sql primary "$LOAD" >/dev/null
check "... the load commits" test "$(rows primary)" == "$ROWS"
check "... the standby replays it" catchup primary standby
check "... and has the same rows" test "$(rows standby)" == "$ROWS"

sql primary "TRUNCATE lt" >/dev/null
catchup primary standby
power_loss "power loss in the first write after a TRUNCATE"
check "... the table is empty" test "$(rows primary)" == 0
sql primary "$LOAD" >/dev/null
check "... the standby replays the load" catchup primary standby
check "... and has the same rows" test "$(rows standby)" == "$ROWS"
echo "1..$NTEST"
finish
