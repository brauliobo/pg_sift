-- ORDER BY a timestamp that arrives in no order LIMIT 10 OFFSET 1000, after a VACUUM FREEZE: the entries of the sorted array of the key before the OFFSET are thrown away without the heap confirming them (their units have no dead row and their pages are all-visible)
-- label: `SELECT id, created_at ORDER BY created_at OFFSET 1000 LIMIT 10`, `order_array_columns = 'created_at'`; the variant `btree` is the same rows with a b-tree on the column
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS topn_offset_btree CASCADE;
CREATE SCHEMA topn_offset_btree;
CREATE TABLE orders_off AS SELECT id, created_at + (hashint8extended(id, 7) & 1048575) * interval '1 microsecond' AS created_at FROM orders;
CREATE INDEX orders_off_col ON orders_off USING sift_col (id, created_at) WITH (order_columns = 'created_at', order_array_columns = 'created_at');
CREATE TABLE topn_offset_btree.orders_off AS TABLE orders_off;
CREATE INDEX orders_off_bt ON topn_offset_btree.orders_off (created_at);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) orders_off;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) topn_offset_btree.orders_off;
--@ pg
SELECT id, created_at FROM orders_off ORDER BY created_at OFFSET 1000 LIMIT 10
