-- the page of a range, ORDER BY a timestamp that arrives in no order LIMIT 20 OFFSET 5000 with a lower bound, over the compact order of the key (order_layout = 'compact'): the bound is found in the buckets and the entries before the OFFSET are thrown away from them
-- label: `SELECT id, created_at WHERE created_at >= '2024-06-01' ORDER BY created_at OFFSET 5000 LIMIT 20`, `order_columns = 'created_at', order_array_columns = 'created_at', order_layout = 'compact'`; the variant `whole` is the same index with the whole order, `btree` the same rows with a b-tree on the column
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS c_order_layout_range__whole CASCADE;
DROP SCHEMA IF EXISTS c_order_layout_range__btree CASCADE;
CREATE SCHEMA c_order_layout_range__whole;
CREATE SCHEMA c_order_layout_range__btree;
CREATE TABLE orders_olr AS SELECT id, timestamp '2024-01-01' + (hashint8extended(id, 7) & 35184372088831) * interval '1 microsecond' AS created_at FROM orders;
CREATE INDEX orders_olr_col ON orders_olr USING sift_col (id, created_at) WITH (order_columns = 'created_at', order_array_columns = 'created_at', order_layout = 'compact');
CREATE TABLE c_order_layout_range__whole.orders_olr AS TABLE orders_olr;
CREATE INDEX orders_olr_col ON c_order_layout_range__whole.orders_olr USING sift_col (id, created_at) WITH (order_columns = 'created_at', order_array_columns = 'created_at', order_layout = 'whole');
CREATE TABLE c_order_layout_range__btree.orders_olr AS TABLE orders_olr;
CREATE INDEX orders_olr_bt ON c_order_layout_range__btree.orders_olr (created_at);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) orders_olr;
-- stop the setup if the key follows the heap: the sizes and times of a clustered key are not those of a scattered one
DO $$ BEGIN IF (SELECT abs(correlation) FROM pg_stats WHERE tablename = 'orders_olr' AND attname = 'created_at') >= 0.05 THEN RAISE EXCEPTION 'created_at follows the heap'; END IF; END $$;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_order_layout_range__whole.orders_olr;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_order_layout_range__btree.orders_olr;
--@ pg
SELECT id, created_at FROM orders_olr WHERE created_at >= timestamp '2024-06-01' ORDER BY created_at OFFSET 5000 LIMIT 20
