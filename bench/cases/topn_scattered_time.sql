-- the n best rows by a time column whose rows are in no order, for an n above the longest head of a run: a superblock answers from the head of its sorted run once the heap holds n rows and its bar is past the head
-- label: `SELECT created_at ORDER BY created_at DESC LIMIT 1000` on a copy of the orders in random order, `order_columns = 'created_at'`, no btree
-- ordered: yes
--@ setup
CREATE TABLE orders_s AS SELECT id, customer_id, created_at, amount FROM orders ORDER BY hashint4(id);
CREATE INDEX orders_s_col ON orders_s USING sift_col (customer_id sift.col_int4_ops, created_at sift.col_timestamp_ops, amount sift.col_numeric_ops) WITH (order_columns = 'created_at');
VACUUM (FREEZE, ANALYZE) orders_s
--@ pg
SELECT created_at FROM orders_s ORDER BY created_at DESC LIMIT 1000
