#!/usr/bin/env bash
# wal_level=minimal: a build and SHARED writes in the creating transaction skip
# WAL and are synced at commit (small relations by WAL-logging them, large ones
# by fsync); both survive kill -9, as do later WAL-logged writes.
. "$(dirname "$0")/lib.bash"

plan 6
node_init min 5 "wal_level = minimal
max_wal_senders = 0
wal_skip_threshold = '1MB'"
node_start min || bail "start failed"
sql min "CREATE EXTENSION sift_test CASCADE" >/dev/null || bail "setup failed"

lsn=$(sql min "SELECT pg_current_wal_insert_lsn()")
sql min >/dev/null <<'SQL' || bail "minimal transaction failed"
BEGIN;
	CREATE TABLE small (v int); INSERT INTO small VALUES (1000), (60000);
	CREATE INDEX small_idx ON small USING sift_store_test (v);
	SELECT sift._test_seg_write('small_idx', 200000, 3);
	CREATE TABLE big (v int); INSERT INTO big VALUES (3000000), (8000000);
	CREATE INDEX big_idx ON big USING sift_store_test (v);
	SELECT sift._test_seg_write('big_idx', 5000000, 4, 0, 64) AS g \gset
	SELECT sift._test_seg_drop('big_idx', :g);
	SELECT sift._test_seg_write('big_idx', 4000000, 5);
COMMIT;
SQL
wal=$(sql min "SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), '$lsn')")
check "the creating transaction skipped WAL for the big index ($wal bytes)" test "$wal" -lt 4000000

VERIFY="SELECT string_agg(i || ':' || n || ':' || ok || ':' || orphans, ' ' ORDER BY i) FROM (
	SELECT i, count(*) n, bool_and(sift._test_seg_read(i, gen) = payload) ok, sift._test_check(i) orphans
	FROM unnest(ARRAY['small_idx', 'big_idx']) i, sift._test_segments(i) GROUP BY i) s"
before=$(sql min "$VERIFY")
crash min
node_start min || bail "restart failed"
check "minimal-WAL build and writes survive kill -9" test "$(sql min "$VERIFY")" == "$before"
diag "$before"

# store records are not transactional: the commit of a transaction that has an
# xid and wrote WAL flushes them (a bare txid_current() commits asynchronously)
sql min "BEGIN" "SELECT txid_current()" \
	"SELECT sift._test_seg_write('big_idx', 3000000, 6), sift._test_seg_write('small_idx', 10000, 7)" \
	"COMMIT" >/dev/null
before=$(sql min "$VERIFY")
crash min
node_start min || bail "restart failed"
check "later WAL-logged writes survive kill -9" test "$(sql min "$VERIFY")" == "$before"

sql min "BEGIN; CREATE TABLE gone (v int); INSERT INTO gone VALUES (50000);
	CREATE INDEX gone_idx ON gone USING sift_store_test (v); SELECT sift._test_seg_write('gone_idx', 100000, 8);
	ROLLBACK;" >/dev/null
crash min
node_start min || bail "restart failed"
check "a rolled-back build leaves nothing behind" test "$(sql min "SELECT count(*) FROM pg_class WHERE relname LIKE 'gone%'")" == 0
check "segments still verify" test "$(sql min "$VERIFY")" == "$before"
check "the sweep finds nothing to reclaim" test "$(sql min "SELECT sift._test_sweep('big_idx') + sift._test_sweep('small_idx')")" == 0
finish
