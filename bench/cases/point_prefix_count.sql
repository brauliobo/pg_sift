-- the rows that start with a prefix, counted: the dictionary names the keys with the prefix and the point directory lists their rows, so the access method answers it (an aggregate scan read every unit the zones kept)
-- label: `count(*) WHERE ref LIKE '<4 characters>%'` over a 64-character key, no btree on `ref`, `point_tid_columns = 'ref'`
-- ordered: yes
-- set: sift.enable_col_scan = on
--@ setup
ALTER TABLE orders ADD COLUMN ref text;
UPDATE orders SET ref = md5(id::text) || md5((id * 7)::text);
ALTER TABLE orders DROP CONSTRAINT orders_pkey;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id sift.col_int4_ops, ref sift.col_text_ops, amount sift.col_numeric_ops) WITH (point_tid_columns = 'ref')
--@ pg
SELECT count(*) AS n FROM orders WHERE ref LIKE 'a1b2%'
