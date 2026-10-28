-- a group by day of a date column that is scattered over the table (see key_cut_day_count)
-- label: `SELECT d, count(*) GROUP BY d ORDER BY d`, 1,000 groups, `sift_col (d)`; the variant `btree` is the same rows with a b-tree and no `sift_col` index
-- ordered: yes
-- set: sift.enable_col_scan = on
--@ setup
DROP SCHEMA IF EXISTS key_cut_group_day_bt CASCADE;
CREATE SCHEMA key_cut_group_day_bt;
CREATE TABLE key_cut_group_day AS SELECT g AS id, date '2022-01-01' + ((hashint8extended(g, 3) & 1073741823) % 1000)::int AS d, (g % 1000)::int AS amount, repeat('x', 40) AS note FROM generate_series(1, 3000000) g;
CREATE INDEX key_cut_group_day_col ON key_cut_group_day USING sift_col (d);
CREATE TABLE key_cut_group_day_bt.key_cut_group_day AS TABLE key_cut_group_day;
CREATE INDEX key_cut_group_day_btree ON key_cut_group_day_bt.key_cut_group_day (d);
VACUUM (FREEZE, ANALYZE) key_cut_group_day;
VACUUM (FREEZE, ANALYZE) key_cut_group_day_bt.key_cut_group_day;
--@ pg
SELECT d, count(*) AS n FROM key_cut_group_day GROUP BY d ORDER BY d
