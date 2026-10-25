-- a group by a text column of 100,000 values over a few hundred rows: the ids of the dictionary's entries are made for the entries the rows read, not for the 100,000 of the dictionary at the start of each statement
-- label: `count(*)`, `sum(amount)` grouped by a text column with 100,000 distinct values, over the rows of a few customers
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN tag text;
UPDATE orders SET tag = 'tag' || (id::bigint * 7919) % 100000;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id, customer_id, amount, tag)
--@ pg
SELECT tag, count(*) AS n, sum(amount) AS revenue FROM orders WHERE customer_id < 6 GROUP BY 1 ORDER BY 1
