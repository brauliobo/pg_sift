-- the rows of one key that is in every unit (a country) within a range of a column that follows the heap (a range of ids: rows that arrived together): the zones of the range cut the units first, and the point directory lists the addresses of the kept units only, so the heap reads the rows of the range, not every row of the key
-- label: `SELECT id, amount, created_at WHERE country = 'TR' AND id BETWEEN <20,000 ids>`, no btree, `point_tid_columns = 'country'`
-- ordered: yes
-- set: sift.enable_col_scan = on
--@ setup
ALTER TABLE orders DROP CONSTRAINT orders_pkey;
DROP INDEX orders_created, orders_country_status;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id sift.col_int4_ops, customer_id sift.col_int4_ops, country sift.col_text_ops, amount sift.col_numeric_ops) WITH (point_tid_columns = 'country')
--@ pg
SELECT id, amount, created_at FROM orders WHERE country = 'TR' AND id BETWEEN 150001 AND 170000 ORDER BY id
