-- a week of a date column that is scattered over the table (see key_cut_day_count)
-- label: `SELECT count(*) WHERE d >= <a day> AND d < <a day> + 7`, `sift_col (d)`; the variant `btree` is the same rows with a b-tree and no `sift_col` index
-- ordered: no
-- set: sift.enable_col_scan = on
--@ setup
DROP SCHEMA IF EXISTS key_cut_week_count_bt CASCADE;
CREATE SCHEMA key_cut_week_count_bt;
CREATE TABLE key_cut_week_count AS SELECT g AS id, date '2022-01-01' + ((hashint8extended(g, 3) & 1073741823) % 1000)::int AS d, (g % 1000)::int AS amount, repeat('x', 40) AS note FROM generate_series(1, 3000000) g;
CREATE INDEX key_cut_week_count_col ON key_cut_week_count USING sift_col (d);
CREATE TABLE key_cut_week_count_bt.key_cut_week_count AS TABLE key_cut_week_count;
CREATE INDEX key_cut_week_count_btree ON key_cut_week_count_bt.key_cut_week_count (d);
VACUUM (FREEZE, ANALYZE) key_cut_week_count;
VACUUM (FREEZE, ANALYZE) key_cut_week_count_bt.key_cut_week_count;
--@ pg
SELECT count(*) FROM key_cut_week_count WHERE d >= date '2022-03-10' AND d < date '2022-03-17'
