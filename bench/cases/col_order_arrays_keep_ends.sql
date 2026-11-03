-- the sorted array of a timestamp column in an index cut by a key: the build keeps the ends of the order of each segment (order_array_columns = 'auto'), a list the user wrote keeps the whole order; a range inside one key reads the units whose zone holds the bound
-- label: `SELECT created_at WHERE customer_id = <one customer> AND created_at >= <middle> ORDER BY created_at LIMIT 20`, `key_cut = 'customer_id'`, scattered microsecond timestamps; the variant `whole` is the same index with `order_array_columns = 'created_at'`
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS col_order_arrays_whole CASCADE;
CREATE SCHEMA col_order_arrays_whole;
CREATE TABLE orders_ak AS SELECT id, customer_id, amount, created_at + (hashint8extended(id, 7) & 1048575) * interval '1 microsecond' AS created_at FROM orders;
CREATE INDEX orders_ak_col ON orders_ak USING sift_col (customer_id, amount, created_at) WITH (key_cut = 'customer_id', order_array_columns = 'auto: created_at');
CREATE TABLE col_order_arrays_whole.orders_ak AS TABLE orders_ak;
CREATE INDEX orders_ak_col ON col_order_arrays_whole.orders_ak USING sift_col (customer_id, amount, created_at) WITH (key_cut = 'customer_id', order_columns = 'created_at', order_array_columns = 'created_at');
VACUUM (FREEZE, ANALYZE) orders_ak;
VACUUM (FREEZE, ANALYZE) col_order_arrays_whole.orders_ak
--@ pg
SELECT created_at FROM orders_ak WHERE customer_id = 137 AND created_at >= '2024-07-01' ORDER BY created_at LIMIT 20
