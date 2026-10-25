-- a page of the rows of a week, ORDER BY a microsecond timestamp that arrives in no order LIMIT 200, no b-tree on it: the sorted array of the key (order_array_columns) is searched for the first entry of the week in each segment and merged from there, where the sorted runs read every unit that holds a row of the week
-- label: `SELECT created_at WHERE created_at >= '2024-09-01' AND created_at < '2024-09-08' ORDER BY created_at DESC LIMIT 200`, `order_columns = 'created_at', order_array_columns = 'created_at'`; the variant `no_array` is the same index with the sorted runs only
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS order_array_range_plain CASCADE;
CREATE SCHEMA order_array_range_plain;
CREATE TABLE orders_or AS SELECT id, amount, created_at + (hashint8extended(id, 7) & 1048575) * interval '1 microsecond' AS created_at FROM orders;
CREATE INDEX orders_or_col ON orders_or USING sift_col (amount, created_at) WITH (order_columns = 'created_at', order_array_columns = 'created_at');
CREATE TABLE order_array_range_plain.orders_or AS TABLE orders_or;
CREATE INDEX orders_or_col ON order_array_range_plain.orders_or USING sift_col (amount, created_at) WITH (order_columns = 'created_at');
VACUUM (FREEZE, ANALYZE) orders_or;
VACUUM (FREEZE, ANALYZE) order_array_range_plain.orders_or;
--@ pg
SELECT created_at FROM orders_or WHERE created_at >= timestamp '2024-09-01' AND created_at < timestamp '2024-09-08' ORDER BY created_at DESC LIMIT 200
