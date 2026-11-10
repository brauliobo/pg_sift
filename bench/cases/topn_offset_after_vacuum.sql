-- an OFFSET over the sorted array of a key after a VACUUM that followed deletes and updates: the entries it throws away are not confirmed in the heap
-- label: `SELECT id, created_at ORDER BY created_at OFFSET rows/10 LIMIT 10` after `DELETE` of 3% and `UPDATE` of 3% of the rows and `VACUUM`; the variant `btree` is the same rows with a b-tree on the column
-- ordered: yes
--@ setup
CREATE TABLE ova AS SELECT id, timestamp '2024-01-01' + (hashint8extended(id, 7) & 35184372088831) * interval '1 microsecond' AS created_at FROM orders ORDER BY md5((id * 31 + 7)::text);
CREATE INDEX ova_col ON ova USING sift_col (id, created_at) WITH (order_array_columns = 'created_at');
VACUUM (FREEZE, ANALYZE) ova;
DELETE FROM ova WHERE id % 33 = 1;
UPDATE ova SET created_at = created_at + interval '1 second' WHERE id % 33 = 2;
VACUUM (ANALYZE) ova;
DROP SCHEMA IF EXISTS c_topn_offset_after_vacuum__btree CASCADE;
CREATE SCHEMA c_topn_offset_after_vacuum__btree;
CREATE TABLE c_topn_offset_after_vacuum__btree.ova AS TABLE ova;
CREATE INDEX ova_bt ON c_topn_offset_after_vacuum__btree.ova (created_at);
INSERT INTO c_topn_offset_after_vacuum__btree.ova SELECT * FROM ova WHERE false;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_topn_offset_after_vacuum__btree.ova;
--@ pg
SELECT id, created_at FROM ova ORDER BY created_at OFFSET {rows} / 10 LIMIT 10
