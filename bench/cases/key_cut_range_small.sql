-- the ranges of a table of 23 superblocks (500,000 rows of 56 bytes) are cut by the start of the range: the default formula has no minimum of units, so a small table is cut as a large one is
-- label: `SELECT count(*) WHERE r @> <a point>` over 500,000 random int4range, `sift_col (r)` (the default class); the variant `gist` is the same rows with a GiST index and no `sift_col` index
-- ordered: no
-- set: sift.enable_col_scan = on
--@ setup
DROP SCHEMA IF EXISTS key_cut_range_small_gist CASCADE;
CREATE SCHEMA key_cut_range_small_gist;
CREATE TABLE key_cut_range_small AS SELECT g AS id, int4range(x, x + 1 + (hashint8extended(g, 7) & 1023)::int) AS r, repeat('x', 40) AS note FROM (SELECT g, (hashint8extended(g, 3) & 33554431)::int AS x FROM generate_series(1, 500000) g) s;
CREATE INDEX key_cut_range_small_col ON key_cut_range_small USING sift_col (r);
CREATE TABLE key_cut_range_small_gist.key_cut_range_small AS TABLE key_cut_range_small;
CREATE INDEX key_cut_range_small_gistx ON key_cut_range_small_gist.key_cut_range_small USING gist (r);
VACUUM (FREEZE, ANALYZE) key_cut_range_small;
VACUUM (FREEZE, ANALYZE) key_cut_range_small_gist.key_cut_range_small;
--@ pg
SELECT count(*) FROM key_cut_range_small WHERE r @> 12345678
