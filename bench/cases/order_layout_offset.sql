-- ORDER BY a timestamp that arrives in no order LIMIT 10 OFFSET 10000 with the compact order of the key (order_layout = 'compact'): the entries before the OFFSET are thrown away from the buckets of their keys, and the index is smaller
-- label: `SELECT id, created_at ORDER BY created_at OFFSET 10000 LIMIT 10`, `order_columns = 'created_at', order_array_columns = 'created_at', order_layout = 'compact'`; the variant `whole` is the same index with the whole order, `btree` the same rows with a b-tree on the column
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS order_layout_offset_whole CASCADE;
DROP SCHEMA IF EXISTS order_layout_offset_btree CASCADE;
CREATE SCHEMA order_layout_offset_whole;
CREATE SCHEMA order_layout_offset_btree;
CREATE TABLE orders_olo AS SELECT id, timestamp '2024-01-01' + (hashint8extended(id, 7) & 35184372088831) * interval '1 microsecond' AS created_at FROM orders;
CREATE INDEX orders_olo_col ON orders_olo USING sift_col (id, created_at) WITH (order_columns = 'created_at', order_array_columns = 'created_at', order_layout = 'compact');
CREATE TABLE order_layout_offset_whole.orders_olo AS TABLE orders_olo;
CREATE INDEX orders_olo_col ON order_layout_offset_whole.orders_olo USING sift_col (id, created_at) WITH (order_columns = 'created_at', order_array_columns = 'created_at', order_layout = 'whole');
CREATE TABLE order_layout_offset_btree.orders_olo AS TABLE orders_olo;
CREATE INDEX orders_olo_bt ON order_layout_offset_btree.orders_olo (created_at);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) orders_olo;
-- stop the setup if the key follows the heap: the sizes and times of a clustered key are not those of a scattered one
SELECT CASE WHEN abs(correlation) < 0.05 THEN 1 ELSE 1 / 0 END FROM pg_stats WHERE tablename = 'orders_olo' AND attname = 'created_at';
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) order_layout_offset_whole.orders_olo;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) order_layout_offset_btree.orders_olo;
--@ pg
SELECT id, created_at FROM orders_olo ORDER BY created_at OFFSET 10000 LIMIT 10
