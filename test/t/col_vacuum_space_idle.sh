#!/usr/bin/env bash
# The free space of a sift_col index nothing changes goes back to the file system within a few VACUUMs, with a standby streaming and the automatic rewrite on.
# VACUUM left the segments that end the file to the next rewrite whenever copying them down wrote more than half of the blocks it gives back, but an index with no
# rewrite coming has nothing to fill the holes (the last pass of the runs of a build, a horizon that held the blocks of a rewrite and passed): the file stood
# at its size for good. The deferral holds only while a rewrite is coming (col_auto_merge_space.sh); an index with none gets its holes back.
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_VSI_ROWS 100000 100000)
plan 6
node_init primary 26 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 512MB
autovacuum = off
sift.test_col_net = off"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" \
	"CREATE TABLE t (id int, grp int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, g % 500, g % 997, md5(g::text) || md5((g * 7)::text) FROM generate_series(1, $ROWS) g" \
	"VACUUM (FREEZE, ANALYZE) t" >/dev/null || bail "setup failed"
node_clone primary standby 27 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"

size() { sql primary "SELECT pg_relation_size('t_idx')"; }
live() { sql primary "SELECT bytes FROM sift.col_index_info('t_idx')"; }
tight() { test "$(size)" -le "$(( $(live) * 120 / 100 ))"; }
# the VACUUM of a table whose rewrite is automatic (the setting of the session is the one VACUUM reads)
vacuum() { sql primary "SET sift.col_auto_merge = on" "VACUUM t" >/dev/null; }
# the standby reports its horizon every second (hot_standby_feedback): a VACUUM moves what the horizon has passed
past() { sql primary "SELECT pg_current_xact_id()" >/dev/null; catchup primary standby; sleep 1.2; }
vacuums=0
vacuum_until_tight() {
	for vacuums in $(seq 12); do
		past
		vacuum
		tight && return 0
	done
	return 1
}
due() { sql primary "SET sift.col_auto_merge = on" "SELECT coalesce(sift.col_maintenance_due('t_idx', true), '-')"; }

sql primary "SET sift.async_maintenance = off" "CREATE INDEX t_idx ON t USING sift_col (id, grp, amount, pad) WITH (key_cut = grp)" >/dev/null || bail "create failed"
for k in 1 2; do
	sql primary "SET sift.async_maintenance = off" "INSERT INTO t SELECT id + $k * 10000000, grp, amount, pad FROM t WHERE id < 10000000 AND id % 5 = $k - 1" \
		"SELECT sift.col_promote('t_idx')" >/dev/null
done
# the merge leaves its output above the segments it replaced: the file is twice the live bytes, and the segments above the holes are as many bytes as the holes
sql primary "SELECT sift.col_merge('t_idx', true)" >/dev/null
past
check "the merge leaves the file over 1.6 times the live bytes" test "$(size)" -gt "$(( $(live) * 16 / 10 ))"
check "... and no rewrite is due" test "$(due)" == "-"
size_loose=$(size)

vacuum_until_tight
check "VACUUM brings the file within 1.2 times the live bytes (standby streaming, automatic rewrite on)" tight
diag "file $size_loose -> $(size) in $vacuums VACUUMs, live $(live)"
check "... within 6 VACUUMs" test "$vacuums" -le 6
check "the primary verifies the index" test -z "$(sql primary "SELECT sift.col_verify('t_idx', true)" 2>&1)"
catchup primary standby
check "... and the standby" test -z "$(sql standby "SELECT sift.col_verify('t_idx', true)" 2>&1)"

finish
