-- how many of 1,000,000 random ranges contain a point, no GiST: the start and end of each range are integer lanes of the sift_col index, so the containment is a pair of integer tests on the zones and the lanes, not the operator on each range
-- label: `SELECT count(*) WHERE r @> 5000000` on 1,000,000 `int4range` of up to 5,000 values from 0 to 10,000,000; the variant `gist` is the same rows with a GiST index and no sift_col index (PostgreSQL's own bitmap scan)
--@ setup
DROP SCHEMA IF EXISTS contain_range_gist CASCADE;
CREATE SCHEMA contain_range_gist;
CREATE TABLE ranges AS SELECT id, int4range(a, a + 1 + (hashint8extended(id, 3) & 1048575)::int % 5000) AS r FROM (SELECT id, ((hashint8extended(id, 7) & 8388607) * 10000000 / 8388608)::int AS a FROM orders) s;
CREATE INDEX ranges_col ON ranges USING sift_col (r);
CREATE TABLE contain_range_gist.ranges AS TABLE ranges;
CREATE INDEX ranges_gist ON contain_range_gist.ranges USING gist (r);
VACUUM (FREEZE, ANALYZE) ranges;
VACUUM (FREEZE, ANALYZE) contain_range_gist.ranges;
--@ pg
SELECT count(*) FROM ranges WHERE r @> 5000000
