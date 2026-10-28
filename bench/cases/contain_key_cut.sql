-- how many of 3,000,000 random ranges contain a point, the units cut by the start of the range (the default of CREATE INDEX on a table whose ranges are scattered over the units) against the units of the superblocks
-- label: `SELECT count(*) WHERE r @> 5000000` on 3,000,000 `int4range` of up to 5,000 values from 0 to 10,000,000 beside an address and a 128-character note, `sift_col (r, ip)` after VACUUM; the variant `no_cut` is the same index with `key_cut = 'off'`, `gist` the same rows with a GiST index and no `sift_col` index
--@ setup
DROP SCHEMA IF EXISTS contain_key_cut_off CASCADE;
DROP SCHEMA IF EXISTS contain_key_cut_gist CASCADE;
CREATE SCHEMA contain_key_cut_off;
CREATE SCHEMA contain_key_cut_gist;
CREATE TABLE cut AS SELECT i AS id, int4range(a, a + 1 + (hashint8extended(i, 3) & 1048575)::int % 5000) AS r, inet '20.0.0.0' + (hashint8extended(i, 5) & 16777215)::int AS ip, repeat(md5(i::text), 4) AS note FROM (SELECT i, ((hashint8extended(i, 7) & 8388607) * 10000000 / 8388608)::int AS a FROM generate_series(1, 3000000) i) s;
CREATE INDEX cut_col ON cut USING sift_col (r, ip);
CREATE TABLE contain_key_cut_off.cut AS SELECT id, r, ip FROM cut;
CREATE INDEX cut_col ON contain_key_cut_off.cut USING sift_col (r, ip) WITH (key_cut = 'off');
CREATE TABLE contain_key_cut_gist.cut AS SELECT id, r, ip FROM cut;
CREATE INDEX cut_gist ON contain_key_cut_gist.cut USING gist (r);
VACUUM (FREEZE, ANALYZE) cut;
VACUUM (FREEZE, ANALYZE) contain_key_cut_off.cut;
VACUUM (FREEZE, ANALYZE) contain_key_cut_gist.cut;
--@ pg
SELECT count(*) FROM cut WHERE r @> 5000000
