-- the last rows of one customer, ORDER BY a microsecond timestamp that arrives in no order DESC LIMIT 10, no b-tree: the arrays are grouped by the column (order_array_group), so the scan reads the entries of the group in each segment where the sorted runs read every unit that holds a row of it
-- label: `SELECT created_at WHERE customer_id = 500 ORDER BY created_at DESC LIMIT 10`, `order_columns = 'created_at', order_array_columns = 'created_at', order_array_group = 'customer_id'`; the variant `no_group` is the same index with the sorted runs only
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS order_array_group_plain CASCADE;
CREATE SCHEMA order_array_group_plain;
CREATE TABLE orders_og AS SELECT id, customer_id, created_at + (hashint8extended(id, 7) & 1048575) * interval '1 microsecond' AS created_at FROM orders;
CREATE INDEX orders_og_col ON orders_og USING sift_col (id, customer_id, created_at) WITH (order_columns = 'created_at', order_array_columns = 'created_at', order_array_group = 'customer_id');
CREATE TABLE order_array_group_plain.orders_og AS TABLE orders_og;
CREATE INDEX orders_og_col ON order_array_group_plain.orders_og USING sift_col (id, customer_id, created_at) WITH (order_columns = 'created_at');
VACUUM (FREEZE, ANALYZE) orders_og;
VACUUM (FREEZE, ANALYZE) order_array_group_plain.orders_og;
--@ pg
SELECT created_at FROM orders_og WHERE customer_id = 500 ORDER BY created_at DESC LIMIT 10
