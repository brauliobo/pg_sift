#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A full disk (sift.test_disk_blocks) under the flush of the staged column deltas of a savepoint: the transaction staged an UPDATE before the savepoint, the savepoint stages enough more for the
# flush to need a block the file does not have. The flush fails, the savepoint is rolled back, and the UPDATE the transaction made before it is still staged: it commits. The partition lock the
# flush took is not held after the rollback, and the rows of the savepoint are not in the table.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 6
node_init cs 100 "autovacuum = off
sift.async_maintenance = off"
node_start cs || bail "start failed"
sql cs "CREATE EXTENSION pg_sift" \
	"CREATE TABLE cd (id bigint, a bigint, b text, c text, d text, e text) USING sift_table" \
	"INSERT INTO cd SELECT g, g, repeat('b', 40), repeat('c', 40), repeat('d', 40), repeat('e', 40) FROM generate_series(1, 20000) g" >/dev/null || bail "setup failed"
blocks=$(sql cs "SELECT pg_relation_size('cd') / 8192")

out=$(sql cs <<SQL 2>&1
\set ON_ERROR_STOP 0
BEGIN;
UPDATE cd SET a = a + 1000000 WHERE id <= 10;
SAVEPOINT sp;
SET sift.test_disk_blocks = $blocks;
UPDATE cd SET a = a + 1000000 WHERE id > 100 AND id <= 12000;
ROLLBACK TO sp;
SET sift.test_disk_blocks = 0;
SELECT 'page_locks ' || count(*) FROM pg_locks WHERE locktype = 'page' AND mode = 'ExclusiveLock' AND pid = pg_backend_pid();
COMMIT;
SQL
)
check "the flush of the savepoint met a full disk" grep -q 'no space left on device' <<<"$out"
check "no partition lock is held after the rollback to the savepoint" grep -qx 'page_locks 0' <<<"$out"
check "the update made before the savepoint is committed" test "$(sql cs "SELECT count(*) FROM cd WHERE id <= 10 AND a > 1000000")" == 10
check "the rows of the savepoint are as they were" test "$(sql cs "SELECT count(*) FROM cd WHERE id > 100 AND a > 1000000")" == 0
check "no other row changed" test "$(sql cs "SELECT count(*) FROM cd WHERE a <> id")" == 10
check "sift.check finds nothing" test "$(sql cs "SELECT count(*) FROM sift.check('cd'::regclass, true)")" == 0
finish
