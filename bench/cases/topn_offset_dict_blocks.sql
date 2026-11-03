-- the strings of a text order array whose dictionary is in compressed blocks, read by code (the last 32 blocks kept): an OFFSET over the sorted array of a key after churn (7% of the rows updated, 1% deleted, a VACUUM): the dead lists of the segments, the heap rows the cut must confirm and the groups of ties
-- label: `ORDER BY t, id LIMIT 20 OFFSET 10000` over a text key over {rows} rows in the order of a hash, an index with the order arrays of the three keys, after an `UPDATE` of 7%, a `DELETE` of 1% and a `VACUUM`; the variant `btree` is the same rows with a b-tree on (t, id)
-- ordered: yes
--@ setup
CREATE TABLE oc AS SELECT id, ((hashint8extended(id, 3) & 2147483647) % {rows})::int AS k, left(md5(id::text), 10) AS t, (date '2024-01-01' + (hashint8extended(id, 9) & 1023)::int % 730) AS d FROM orders ORDER BY md5((id * 31 + 7)::text);
CREATE INDEX oc_col ON oc USING sift_col (id, k, t, d) WITH (order_columns = 'k, d', order_array_columns = 'k, t, d');
VACUUM (FREEZE, ANALYZE) oc;
UPDATE oc SET k = k + 1 WHERE id % 14 = 1;
DELETE FROM oc WHERE id % 100 = 3;
VACUUM (ANALYZE) oc;
DROP SCHEMA IF EXISTS c_topn_offset_dict_blocks__btree CASCADE;
CREATE SCHEMA c_topn_offset_dict_blocks__btree;
CREATE TABLE c_topn_offset_dict_blocks__btree.oc AS TABLE oc;
CREATE INDEX oc_bt ON c_topn_offset_dict_blocks__btree.oc (t, id);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_topn_offset_dict_blocks__btree.oc
--@ pg
SELECT id, t FROM oc ORDER BY t, id LIMIT 20 OFFSET 10000
