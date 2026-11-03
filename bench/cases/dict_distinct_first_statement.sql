-- the first statement of a new backend on a text column of as many distinct 32-byte strings as rows, with the order array of the column: the first backend parses the dictionary into the shared ring, later ones read it where it lies
-- label: `count(*)` where a text column with one distinct md5 string per row equals one of them, the first statement of a new backend; `no_ring` is `sift.col_dict_shared_cache_kb = 0` (each backend parses its own), `warm_backend` the same statement as the second of a backend
-- ordered: yes
-- fresh: yes
--@ setup
ALTER TABLE orders ADD COLUMN tag text;
UPDATE orders SET tag = md5(id::text);
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id, customer_id, amount, tag) WITH (order_array_columns = 'tag')
--@ pg
SELECT count(*), sum(amount) FROM orders WHERE tag = md5('777')
