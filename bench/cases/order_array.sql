-- ORDER BY a microsecond timestamp that arrives in no order LIMIT 200, no b-tree on it: the sorted array of the key's values and heap addresses of each segment (order_array_columns) is merged for the first rows, where the sorted runs still read a unit for each of them
-- label: `SELECT created_at ORDER BY created_at DESC LIMIT 200`, `order_columns = 'created_at', order_array_columns = 'created_at'`; the variant `no_array` is the same index with the sorted runs only
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS order_array_plain CASCADE;
CREATE SCHEMA order_array_plain;
CREATE TABLE orders_oa AS SELECT id, amount, created_at + (hashint8extended(id, 7) & 1048575) * interval '1 microsecond' AS created_at FROM orders;
CREATE INDEX orders_oa_col ON orders_oa USING sift_col (amount, created_at) WITH (order_columns = 'created_at', order_array_columns = 'created_at');
CREATE TABLE order_array_plain.orders_oa AS TABLE orders_oa;
CREATE INDEX orders_oa_col ON order_array_plain.orders_oa USING sift_col (amount, created_at) WITH (order_columns = 'created_at');
VACUUM (FREEZE, ANALYZE) orders_oa;
VACUUM (FREEZE, ANALYZE) order_array_plain.orders_oa;
--@ pg
SELECT created_at FROM orders_oa ORDER BY created_at DESC LIMIT 200
