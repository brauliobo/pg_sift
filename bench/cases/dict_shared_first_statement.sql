-- the first statement of a backend over a text column of 100,000 values of 37 bytes that groups every row: the dictionary another backend parsed is copied from shared memory, not inflated and indexed again
-- label: `count(*)` grouped by a text column with 100,000 distinct values, the first statement of a new backend
-- ordered: yes
-- fresh: yes
--@ setup
ALTER TABLE orders ADD COLUMN tag text;
UPDATE orders SET tag = 'item-' || md5(((id::bigint * 7919) % 100000)::text);
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id, customer_id, amount, tag)
--@ pg
SELECT tag, count(*) AS n FROM orders GROUP BY 1 ORDER BY n DESC, tag LIMIT 10
