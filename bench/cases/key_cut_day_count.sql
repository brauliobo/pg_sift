-- the rows of a date column, 3,000,000 rows of 56 bytes whose 1,000 days are scattered over the table, with the plain sift_col index and no key_cut: the default formula cuts the units by the date, so a day is read from the few units of its range
-- label: `SELECT count(*) WHERE d = <a day>`, `sift_col (d)`; the variant `btree` is the same rows with a b-tree and no `sift_col` index
-- ordered: no
-- set: sift.enable_col_scan = on
--@ setup
DROP SCHEMA IF EXISTS key_cut_day_count_bt CASCADE;
CREATE SCHEMA key_cut_day_count_bt;
CREATE TABLE key_cut_day_count AS SELECT g AS id, date '2022-01-01' + ((hashint8extended(g, 3) & 1073741823) % 1000)::int AS d, (g % 1000)::int AS amount, repeat('x', 40) AS note FROM generate_series(1, 3000000) g;
CREATE INDEX key_cut_day_count_col ON key_cut_day_count USING sift_col (d);
CREATE TABLE key_cut_day_count_bt.key_cut_day_count AS TABLE key_cut_day_count;
CREATE INDEX key_cut_day_count_btree ON key_cut_day_count_bt.key_cut_day_count (d);
VACUUM (FREEZE, ANALYZE) key_cut_day_count;
VACUUM (FREEZE, ANALYZE) key_cut_day_count_bt.key_cut_day_count;
--@ pg
SELECT count(*) FROM key_cut_day_count WHERE d = date '2022-03-10'
