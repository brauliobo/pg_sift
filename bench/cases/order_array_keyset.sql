-- the page after a key, ORDER BY a microsecond timestamp that arrives in no order and the id, LIMIT 50, no b-tree: the sorted array of the key (order_array_columns) is searched for the key of the page and the rows are read from there in groups of the first key, where the sorted runs read every unit that holds a row after the key
-- label: `SELECT id, created_at WHERE (created_at, id) > ('2024-09-01', 100000) ORDER BY created_at, id LIMIT 50`, `order_columns = 'created_at', order_array_columns = 'created_at'`; the variant `no_array` is the same index with the sorted runs only
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS order_array_keyset_plain CASCADE;
CREATE SCHEMA order_array_keyset_plain;
CREATE TABLE orders_ok AS SELECT id, amount, created_at + (hashint8extended(id, 7) & 1048575) * interval '1 microsecond' AS created_at FROM orders;
CREATE INDEX orders_ok_col ON orders_ok USING sift_col (id, amount, created_at) WITH (order_columns = 'created_at', order_array_columns = 'created_at');
CREATE TABLE order_array_keyset_plain.orders_ok AS TABLE orders_ok;
CREATE INDEX orders_ok_col ON order_array_keyset_plain.orders_ok USING sift_col (id, amount, created_at) WITH (order_columns = 'created_at');
VACUUM (FREEZE, ANALYZE) orders_ok;
VACUUM (FREEZE, ANALYZE) order_array_keyset_plain.orders_ok;
--@ pg
SELECT id, created_at FROM orders_ok WHERE (created_at, id) > (timestamp '2024-09-01', 100000) ORDER BY created_at, id LIMIT 50
