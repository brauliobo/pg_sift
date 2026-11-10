-- a page under a condition on another column (WHERE deleted_at IS NULL, 2% of the rows set) deep in the sorted array of a key after churn: the entries before the OFFSET are tested on the values of the units and only the entries the units cannot say are fetched from the heap
-- label: `WHERE deleted_at IS NULL ORDER BY d DESC, id LIMIT 20 OFFSET 10000` over a date key over {rows} rows in the order of a hash, an index with the order arrays of the three keys, after an `UPDATE` of 7%, a `DELETE` of 1%, an `UPDATE` of 2% that sets `deleted_at` and a `VACUUM`; the variant `btree` is the same rows with a b-tree on (k, id)
-- ordered: yes
--@ setup
CREATE TABLE oc AS SELECT id, ((hashint8extended(id, 3) & 2147483647) % {rows})::int AS k, left(md5(id::text), 10) AS t, (date '2024-01-01' + (hashint8extended(id, 9) & 1023)::int % 730) AS d, NULL::timestamptz AS deleted_at FROM orders ORDER BY md5((id * 31 + 7)::text);
CREATE INDEX oc_col ON oc USING sift_col (id, k, t, d, deleted_at) WITH (order_array_columns = 'k, t, d');
VACUUM (FREEZE, ANALYZE) oc;
UPDATE oc SET k = k + 1 WHERE id % 14 = 1;
UPDATE oc SET deleted_at = now() WHERE id % 50 = 5;
DELETE FROM oc WHERE id % 100 = 3;
VACUUM (ANALYZE) oc;
DROP SCHEMA IF EXISTS c_topn_offset_filter_date__btree CASCADE;
CREATE SCHEMA c_topn_offset_filter_date__btree;
CREATE TABLE c_topn_offset_filter_date__btree.oc AS TABLE oc;
CREATE INDEX oc_bt ON c_topn_offset_filter_date__btree.oc (d DESC, id);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_topn_offset_filter_date__btree.oc
--@ pg
SELECT id, d FROM oc WHERE deleted_at IS NULL ORDER BY d DESC, id LIMIT 20 OFFSET 10000
