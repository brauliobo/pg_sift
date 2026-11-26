#!/usr/bin/env bash
# A full disk under Format 2 (sift.test_disk_blocks: the file of an index may not grow past that many blocks, an extension fails as ENOSPC does): a statement that fails gives back the blocks
# it took, so that a retry after the space returns takes them (the used blocks, the high-water mark less the free ones, never grow with the number of failures); a rolled back savepoint leaves
# no extension lock, and the statements after it in the same transaction run; sift.check finds nothing at any step; the rows are exact. The wide values of a sift_table that meet the full
# disk give their extents back too, though the table was swept already and no VACUUM would find them as orphans before a restart.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 16
node_init en 93 "autovacuum = off
sift.on_corruption = error"
node_start en || bail "start failed"
sql en "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "CREATE TABLE h (id int, k int, v int)" \
	"INSERT INTO h SELECT i, (random() * 1e6)::int, (random() * 1e9)::int FROM generate_series(1, 100000) i" \
	"CREATE INDEX h_idx ON h USING sift_col (id, k, v)" >/dev/null || bail "setup failed"

space() { sql en "SELECT $1 FROM sift.space('h_idx')"; }
used() { space used_blocks; }
eof() { space relation_blocks; }
problems() { sql en "SELECT count(*) FROM sift.check('h_idx')"; }
more() { echo "INSERT INTO h SELECT i, (random() * 1e6)::int, (random() * 1e9)::int FROM generate_series($1, $2) i"; }
fails_full() { grep -q 'no space left on device' <<<"$1"; }

base=$(used)
rows=100000
full=$(eof)
out=$(sql en "SET sift.test_disk_blocks = $full" "$(more 100001 300000)" 2>&1) || true
check "an INSERT that needs a block the disk has not fails as a full disk does" fails_full "$out"
first=$(used)
check "and gives back the blocks it took" test "$first" -le "$base"
for i in 1 2 3 4; do sql en "SET sift.test_disk_blocks = $full" "$(more 100001 300000)" >/dev/null 2>&1 || true; done
check "four more failures take no block for good" test "$(used)" -le "$first"
check "the rows are the ones that were committed" test "$(sql en "SELECT count(*) FROM h")" == $rows
check "sift.check finds nothing" test "$(problems)" == 0

sql en "$(more 100001 300000)" >/dev/null || bail "the insert after the space returned failed"
rows=300000
check "after the space returns the INSERT runs and the rows are exact" test "$(sql en "SELECT count(*) FROM h")" == $rows
check "sift.check finds nothing after it" test "$(problems)" == 0

full=$(eof)
used_before=$(used)
out=$(sql en <<SQL 2>&1
\set ON_ERROR_STOP 0
BEGIN;
SAVEPOINT s;
SET sift.test_disk_blocks = $full;
SELECT sift.maintain('h_idx');
ROLLBACK TO s;
SELECT 'locks ' || count(*) FROM pg_locks WHERE locktype = 'extend' AND pid = pg_backend_pid();
SELECT 'used ' || used_blocks FROM sift.space('h_idx');
SET sift.test_disk_blocks = 0;
SELECT 'maintained' FROM (SELECT sift.maintain('h_idx')) m;
$(more 300001 310000);
SELECT 'rows ' || count(*) FROM h;
COMMIT;
SQL
)
check "a savepoint that met a full disk in sift.maintain is rolled back" fails_full "$out"
check "and no extension lock is held after it" grep -qx 'locks 0' <<<"$out"
check "and the blocks it took are given back" test "$(sed -n 's/^used //p' <<<"$out")" -le "$used_before"
check "and the statements after it in the transaction run" grep -qx 'rows 310000' <<<"$out"
rows=310000

out=$(sql en <<SQL 2>&1
\set ON_ERROR_STOP 0
BEGIN;
SAVEPOINT s;
SET sift.test_disk_blocks = 20;
CREATE INDEX h_idx2 ON h USING sift_col (id, k, v);
ROLLBACK TO s;
SELECT 'locks ' || count(*) FROM pg_locks WHERE locktype = 'extend' AND pid = pg_backend_pid();
SET sift.test_disk_blocks = 0;
CREATE INDEX h_idx2 ON h USING sift_col (id, k, v);
COMMIT;
SQL
)
check "a CREATE INDEX that met a full disk in a savepoint leaves no extension lock and can be run again in the transaction" test "$(grep -c '^locks 0$' <<<"$out")" == 1 -a "$(sql en "SELECT count(*) FROM pg_class WHERE relname = 'h_idx2'")" == 1
check "sift.check finds nothing in either index at the end" test "$(sql en "SELECT count(*) FROM sift.check('h_idx2')")" == 0 -a "$(problems)" == 0

sql en "CREATE FUNCTION wide(int) RETURNS text LANGUAGE sql AS \$\$ SELECT string_agg(md5(random()::text || \$1 || k), '') FROM generate_series(1, 300) k \$\$" \
	"SET sift.test_create_format1 = off" "CREATE TABLE w (id int, body text) USING sift_table" "INSERT INTO w SELECT g, wide(g) FROM generate_series(1, 50) g" "VACUUM w" >/dev/null || bail "setup of w failed"
wused() { sql en "SELECT used_blocks FROM sift.space('w')"; }
before=$(wused)
out=$(sql en "SET sift.test_disk_blocks = $(sql en "SELECT relation_blocks FROM sift.space('w')")" "INSERT INTO w SELECT g, wide(g) FROM generate_series(1, 400) g" 2>&1) || true
check "an INSERT of wide values into a sift_table fails as a full disk does" fails_full "$out"
check "and gives the extents of its values back ($before -> $(wused) blocks in use)" test "$(wused)" -le "$before"
check "the table keeps its rows and sift.check finds nothing in it" test "$(sql en "SELECT count(*) FROM w")|$(sql en "SELECT count(*) FROM sift.check('w')")" == "50|0"
finish
