-- the rows of 1,000,000 random ranges that contain a point, with no b-tree or GiST: the plain sift_col index returns them from the lanes of the range, where the native plan is a sequential scan
-- label: `SELECT id, r WHERE r @> 5000000` returning about 250 rows, `sift_col (r)` (the default class); the native lane is the same table with `sift.enable_col_scan = off`; the variant `gist` is the same rows with a GiST index and no `sift_col` index
-- ordered: no
-- set: sift.enable_col_scan = on
--@ setup
DROP SCHEMA IF EXISTS contain_rows_gist CASCADE;
CREATE SCHEMA contain_rows_gist;
CREATE TABLE ranges_rows AS SELECT id, int4range(a, a + 1 + (hashint8extended(id, 3) & 1048575)::int % 5000) AS r FROM (SELECT id, ((hashint8extended(id, 7) & 8388607) * 10000000 / 8388608)::int AS a FROM orders) s;
CREATE INDEX ranges_rows_col ON ranges_rows USING sift_col (r);
CREATE TABLE contain_rows_gist.ranges_rows AS TABLE ranges_rows;
CREATE INDEX ranges_rows_gist ON contain_rows_gist.ranges_rows USING gist (r);
VACUUM (FREEZE, ANALYZE) ranges_rows;
VACUUM (FREEZE, ANALYZE) contain_rows_gist.ranges_rows;
--@ pg
SELECT id, r FROM ranges_rows WHERE r @> 5000000
