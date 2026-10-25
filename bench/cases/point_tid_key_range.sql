-- a key and a range on another column, on a table written in (key, time) order with wide rows: the point directory lists the key's heap addresses, and a unit that lists many of them is read and filtered, so the heap is handed the rows of the range and no more
-- label: `SELECT id, pad WHERE customer_id = <a customer of 600 rows> AND created_at` in a month, rows of 1,000 bytes written by `(customer_id, created_at)`, no btree, `point_tid_columns = 'customer_id'`, `sift.enable_col_scan`
-- ordered: no
-- set: sift.enable_col_scan = on
--@ setup
CREATE TABLE orders_k AS SELECT id, customer_id, created_at, amount, repeat('x', 1000) AS pad FROM orders WHERE id <= 300000 ORDER BY customer_id, created_at;
CREATE INDEX orders_k_col ON orders_k USING sift_col (customer_id sift.col_int4_ops, created_at sift.col_timestamp_ops, amount sift.col_numeric_ops) WITH (point_tid_columns = 'customer_id');
VACUUM (FREEZE, ANALYZE) orders_k
--@ pg
SELECT id, pad FROM orders_k WHERE customer_id = 1 AND created_at >= timestamp '2024-03-01' AND created_at < timestamp '2024-06-01'
