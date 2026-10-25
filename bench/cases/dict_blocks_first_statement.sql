-- the first statement of a backend over a text column of 100,000 values of 37 bytes, 82 rows: the dictionary stays compressed in the segment and the blocks of the strings of the groups are decompressed, not the 100,000
-- label: `count(*)`, `sum(amount)` grouped by a text column with 100,000 distinct values over 82 rows, the first statement of a new backend
-- ordered: yes
-- fresh: yes
--@ setup
ALTER TABLE orders ADD COLUMN tag text;
UPDATE orders SET tag = 'item-' || md5(((id::bigint * 7919) % 100000)::text);
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id, customer_id, amount, tag)
--@ pg
SELECT tag, count(*) AS n, sum(amount) AS revenue FROM orders WHERE id BETWEEN 5000 AND 5081 GROUP BY 1 ORDER BY 1
