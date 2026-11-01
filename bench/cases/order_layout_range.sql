-- the page of a range, ORDER BY a timestamp that arrives in no order LIMIT 20 OFFSET 5000 with a lower bound, over the compact order of the key (order_layout = 'compact'): the bound is found in the buckets and the entries before the OFFSET are thrown away from them
-- label: `SELECT id, created_at WHERE created_at >= '2024-06-01' ORDER BY created_at OFFSET 5000 LIMIT 20`, `order_columns = 'created_at', order_array_columns = 'created_at', order_layout = 'compact'`; the variant `whole` is the same index with the whole order, `btree` the same rows with a b-tree on the column
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS order_layout_range_whole CASCADE;
DROP SCHEMA IF EXISTS order_layout_range_btree CASCADE;
CREATE SCHEMA order_layout_range_whole;
CREATE SCHEMA order_layout_range_btree;
CREATE TABLE orders_olr AS SELECT id, timestamp '2024-01-01' + (hashint8extended(id, 7) & 35184372088831) * interval '1 microsecond' AS created_at FROM orders;
CREATE INDEX orders_olr_col ON orders_olr USING sift_col (id, created_at) WITH (order_columns = 'created_at', order_array_columns = 'created_at', order_layout = 'compact');
CREATE TABLE order_layout_range_whole.orders_olr AS TABLE orders_olr;
CREATE INDEX orders_olr_col ON order_layout_range_whole.orders_olr USING sift_col (id, created_at) WITH (order_columns = 'created_at', order_array_columns = 'created_at', order_layout = 'whole');
CREATE TABLE order_layout_range_btree.orders_olr AS TABLE orders_olr;
CREATE INDEX orders_olr_bt ON order_layout_range_btree.orders_olr (created_at);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) orders_olr;
-- stop the setup if the key follows the heap: the sizes and times of a clustered key are not those of a scattered one
SELECT CASE WHEN abs(correlation) < 0.05 THEN 1 ELSE 1 / 0 END FROM pg_stats WHERE tablename = 'orders_olr' AND attname = 'created_at';
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) order_layout_range_whole.orders_olr;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) order_layout_range_btree.orders_olr;
--@ pg
SELECT id, created_at FROM orders_olr WHERE created_at >= timestamp '2024-06-01' ORDER BY created_at OFFSET 5000 LIMIT 20
