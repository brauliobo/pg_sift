-- an equality on an id through a packed point directory (column format minor 35): the entries of the id by bucket, 3.6 bytes a row where the slots took 10.7, the same two blocks to read
-- label: `SELECT id, amount, title WHERE id = 410001`, no btree on `id`, `point_tid_columns = 'id'`
-- ordered: yes
-- set: sift.enable_col_scan = on
--@ setup
ALTER TABLE orders DROP CONSTRAINT orders_pkey;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id sift.col_int4_ops, customer_id sift.col_int4_ops, amount sift.col_numeric_ops) WITH (point_tid_columns = 'id')
--@ pg
SELECT id, customer_id, amount, title FROM orders WHERE id = 410001 ORDER BY id
