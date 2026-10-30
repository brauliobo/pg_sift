-- a page of the strings from a bound, ORDER BY a text column LIMIT 20, no b-tree on it: the sorted array of the strings (order_array_columns) is searched for the first string of the range in each segment by comparing strings, where the units read every string of the column
-- label: `SELECT code WHERE code >= 'c' ORDER BY code LIMIT 20` over 32-character strings, `order_array_columns = 'code'`; the variant `no_array` is the same index without the array
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS order_array_text_range_plain CASCADE;
CREATE SCHEMA order_array_text_range_plain;
CREATE TABLE orders_tx AS SELECT id, amount, md5(id::text) AS code FROM orders;
CREATE INDEX orders_tx_col ON orders_tx USING sift_col (amount, code) WITH (order_array_columns = 'code');
CREATE TABLE order_array_text_range_plain.orders_tx AS TABLE orders_tx;
CREATE INDEX orders_tx_col ON order_array_text_range_plain.orders_tx USING sift_col (amount, code) WITH (order_array_columns = 'off');
VACUUM (FREEZE, ANALYZE) orders_tx;
VACUUM (FREEZE, ANALYZE) order_array_text_range_plain.orders_tx;
--@ pg
SELECT code FROM orders_tx WHERE code >= 'c' ORDER BY code LIMIT 20
