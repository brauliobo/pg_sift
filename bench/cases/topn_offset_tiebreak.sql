-- a deep OFFSET over the sorted array of a key with a tie-break key after it: the groups of equal first keys wholly before the offset are counted from the array
-- label: `SELECT id, d ORDER BY d DESC, id OFFSET rows/200 LIMIT 20` over a date with 730 values (about 1,400 rows a value at 1,000,000 rows); the variant `btree` is a b-tree on (d, id)
-- ordered: yes
--@ setup
CREATE TABLE ott AS SELECT id, (date '2024-01-01' + (hashint8extended(id, 9) & 1023)::int % 730) AS d FROM orders ORDER BY md5((id * 31 + 7)::text);
CREATE INDEX ott_col ON ott USING sift_col (id, d) WITH (order_columns = 'd', order_array_columns = 'd');
DROP SCHEMA IF EXISTS c_topn_offset_tiebreak__btree CASCADE;
CREATE SCHEMA c_topn_offset_tiebreak__btree;
CREATE TABLE c_topn_offset_tiebreak__btree.ott AS TABLE ott;
CREATE INDEX ott_bt ON c_topn_offset_tiebreak__btree.ott (d, id);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) ott;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_topn_offset_tiebreak__btree.ott;
--@ pg
SELECT id, d FROM ott ORDER BY d DESC, id OFFSET {rows} / 200 LIMIT 20
