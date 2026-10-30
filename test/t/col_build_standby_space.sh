#!/usr/bin/env bash
# A parallel build leaves the segments of its participants over the same rows and keys, so a rewrite follows (3.6.0: the first VACUUM), and the rewrite wrote its
# output after the segments it replaced: the file was twice the index, and VACUUM does not cut it while a standby streams.
# The build now reserves a gap in front of the segments of its participants, merges them into it and cuts the file behind it, in the file no query of a standby can
# hold before the commit: the file is the index right after CREATE INDEX and REINDEX, and nothing is left for the first VACUUM. Gap sizes: by the statistics (the
# default), none (the space step brings the merge down), and one block-sized sliver (the output lands mostly above the segments it replaces).
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_BSS_ROWS 1000000 500000)
plan 19
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 256MB
autovacuum = off
max_worker_processes = 16
max_parallel_workers = 8"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" \
	"CREATE TABLE t (id int, grp int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, g % 500, g % 997, md5(g::text) || md5((g * 7)::text) FROM generate_series(1, $ROWS) g" \
	"VACUUM (FREEZE, ANALYZE) t" >/dev/null || bail "setup failed"
node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"

OPTS="SET max_parallel_maintenance_workers = 4"
SMALL="SET min_parallel_table_scan_size = 0"
size() { sql primary "SELECT pg_relation_size('t_idx')"; }
live() { sql primary "SELECT bytes FROM sift.col_index_info('t_idx')"; }
# PCT: the most the file may be over its live bytes, in percent (the default 110), and 32 blocks (256 kB): the metadata, the free list and the few free blocks a merge
# leaves are 3 to 13 blocks whatever the size (12 REINDEXes of a 160-block index: 3 with the gap of the statistics, 3 to 13 with a gap of 0 or 8 blocks; 18 on a CI runner),
# which is 2% to 11% of an index of 160 blocks, and the bound is for a file that doubles
tight() { echo "# file $(size) live $(live)" >&2; test "$(size)" -le "$(( $(live) * ${1:-110} / 100 + 32 * 8192 ))"; }

sql primary "$OPTS" "$SMALL" "CREATE INDEX t_idx ON t USING sift_col (id, grp, amount) WITH (key_cut = grp)" >/dev/null || bail "create index failed"
check "a standby streams: the file of CREATE INDEX is at most 1.1 times its live bytes" tight
# the rewrite that the first VACUUM asks the worker for, now
sql primary "SELECT sift.col_maintain('t_idx')" "VACUUM t" >/dev/null || bail "rewrite failed"
check "... and the rewrite of the first VACUUM does not grow it" tight
check "... the segments were rewritten by the build (nothing is left to rewrite)" test "$(sql primary "SELECT coalesce(sift.col_maintenance_due('t_idx', false), 'none')")" == none
sql primary "$OPTS" "$SMALL" "REINDEX INDEX t_idx" >/dev/null || bail "reindex failed"
check "REINDEX: the file is at most 1.1 times its live bytes" tight
sql primary "SELECT sift.col_maintain('t_idx')" "VACUUM t" >/dev/null || bail "rewrite failed"
check "... and the rewrite of the first VACUUM does not grow it" tight
# the new index of a concurrent build is not ready, so no query reads it and the cut of its file takes no lock that a standby replays
sql primary "$OPTS" "$SMALL" "REINDEX INDEX CONCURRENTLY t_idx" >/dev/null || bail "concurrent reindex failed"
check "REINDEX CONCURRENTLY: the file is at most 1.1 times its live bytes" tight
check "... nothing is left to rewrite" test "$(sql primary "SELECT coalesce(sift.col_maintenance_due('t_idx', false), 'none')")" == none
# writers hold a RowExclusiveLock on the new index for the length of each statement: the cut asks for its lock again until they leave a gap
printf 'INSERT INTO t VALUES (-1, 7, 1, $$w$$);\n' >"$(dir primary)/w.sql"
pgbench_bg primary 2 120 -f "$(dir primary)/w.sql"
WRITERS=$PGBENCH_PID
sleep 1
sql primary "$OPTS" "$SMALL" "REINDEX INDEX CONCURRENTLY t_idx" >/dev/null || bail "concurrent reindex under writers failed"
kill "$WRITERS" 2>/dev/null; wait "$WRITERS" 2>/dev/null
# the rows the writers added since the build are in write buffers, which are live data that "live bytes" (the column segments) does not count: 0.3 to 0.5 MB at the pace
# of a lab host (six runs, the file 1.01 times the segments and buffers); on a CI runner the file was 1.9 MB over its live bytes
buffers() { sql primary "SELECT coalesce(sum(bytes), 0) FROM sift.col_segments('t_idx') WHERE kind <> 'immutable'"; }
BUF=$(buffers)
echo "# writers: file $(size) live $(live) write buffers $BUF" >&2
check "REINDEX CONCURRENTLY under writers: the file is at most 1.1 times its live bytes and write buffers" test "$(size)" -le "$(( ($(live) + BUF) * 110 / 100 ))"
# promoting the buffers leaves their pages free (held until the horizon passes), a hole of the size of the buffers at most twice
sql primary "SET sift.async_maintenance = off" "SELECT sift.col_maintain('t_idx')" >/dev/null
echo "# promoted: file $(size) live $(live)" >&2
check "... and after the buffers are promoted at most 1.1 times its live bytes and twice the buffers" test "$(size)" -le "$(( $(live) * 110 / 100 + 2 * BUF ))"
for gap in 0 8; do
	sql primary "SET sift.test_col_gap_blocks = $gap" "$OPTS" "$SMALL" "REINDEX INDEX t_idx" >/dev/null || bail "reindex with gap $gap failed"
	check "a gap of $gap blocks: nothing is left to rewrite" test "$(sql primary "SELECT coalesce(sift.col_maintenance_due('t_idx', false), 'none')")" == none
	if ((gap == 0)); then
		check "... the file is at most 1.1 times its live bytes" tight
	else
		# the output of a sliver of a gap lands above the free blocks it leaves, and the space step fills the holes that fit it: a few blocks stay free
		check "... the file is at most 1.25 times its live bytes (the space step brings the output down)" tight 125
	fi
	check "... the index counts as the table" test "$(sql primary "SET enable_seqscan = off" "SELECT count(*), sum(amount) FROM t WHERE grp = 7")" == "$(sql primary "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SELECT count(*), sum(amount) FROM t WHERE grp = 7")"
done
check "the index counts as the table" test "$(sql primary "SET enable_seqscan = off" "SELECT count(*), sum(amount) FROM t WHERE grp = 7")" == "$(sql primary "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SELECT count(*), sum(amount) FROM t WHERE grp = 7")"
catchup primary standby
check "the standby counts as the primary" test "$(sql standby "SELECT count(*), sum(amount) FROM t")" == "$(sql primary "SELECT count(*), sum(amount) FROM t")"
check "the standby verifies the index" test -z "$(sql standby "SELECT sift.col_verify('t_idx', true)" 2>&1)"
check "the primary verifies the index" test -z "$(sql primary "SELECT sift.col_verify('t_idx', true)" 2>&1)"
finish
