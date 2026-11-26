#!/usr/bin/env bash
# tap: pooled
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# Many narrow UPDATEs of one part and granule are written to the column-delta chain as batched records. Readers see the rows as they were: an old snapshot the old values, a new one the new,
# a point fetch the row, a savepoint rollback none of its rows, a string column the NULLs of its rows; the fold of the chain keeps them, and the table is the same as the heap twin that took
# the same statements.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 8
node_init cb 101 "autovacuum = off
sift.async_maintenance = off"
node_start cb || bail "start failed"
sql cb "CREATE EXTENSION pg_sift" \
	"CREATE TABLE cb (id bigint, a bigint, b double precision, c text) USING sift_table" \
	"CREATE TABLE hp (id bigint, a bigint, b double precision, c text)" \
	"INSERT INTO cb SELECT g, g, g / 2.0, repeat('c', 40) FROM generate_series(1, 20000) g" \
	"INSERT INTO hp SELECT * FROM cb" >/dev/null || bail "setup failed"

sql cb "UPDATE cb SET a = a + 1000000, b = b + 1 WHERE id <= 3000" \
	"UPDATE hp SET a = a + 1000000, b = b + 1 WHERE id <= 3000" >/dev/null
sql cb "UPDATE cb SET a = a + 7 WHERE id % 3 = 0 AND id <= 9000" \
	"UPDATE hp SET a = a + 7 WHERE id % 3 = 0 AND id <= 9000" >/dev/null

same() { test "$(sql cb "SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM cb t")" == "$(sql cb "SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM hp t")"; }

check "the table is the heap twin after the updates" same
for t in cb hp; do
	sql cb "UPDATE $t SET c = CASE WHEN id % 4 = 0 THEN NULL WHEN id % 7 = 0 THEN 'x' || id ELSE 'shipped' END WHERE id <= 6000" >/dev/null
done
check "a batch of strings some of whose rows are set to NULL reads back as the heap twin" same
check "a point fetch sees the last write of the row" test "$(sql cb "SELECT a FROM cb WHERE id = 3")" == "$(sql cb "SELECT a FROM hp WHERE id = 3")"
check "sift.check finds nothing" test "$(sql cb "SELECT count(*) FROM sift.check('cb'::regclass, true)")" == 0

sql cb <<SQL >/dev/null 2>&1
BEGIN ISOLATION LEVEL REPEATABLE READ;
UPDATE cb SET a = a + 1 WHERE id <= 5000;
ROLLBACK;
SQL
check "a rolled back update leaves the sum as it was" test "$(sql cb "SELECT sum(a) FROM cb")" == "$(sql cb "SELECT sum(a) FROM hp")"

sql cb <<SQL >/dev/null 2>&1
BEGIN;
UPDATE cb SET a = a + 5 WHERE id <= 2000;
SAVEPOINT sp;
UPDATE cb SET a = a + 5 WHERE id <= 2000;
ROLLBACK TO sp;
COMMIT;
SQL
sql cb "UPDATE hp SET a = a + 5 WHERE id <= 2000" >/dev/null
check "the rows of a savepoint rolled back are not in the table" same

sql cb "SELECT sift.maintain('cb'::regclass)" >/dev/null 2>&1
check "the fold of the chain keeps the rows" same
check "sift.check finds nothing after the fold" test "$(sql cb "SELECT count(*) FROM sift.check('cb'::regclass, true)")" == 0
finish
