-- a count and an average by a key over the rows whose soft-delete stamp is NULL: a unit that holds one stamped row answers from its pre-aggregates minus the pre-aggregates of that row, where the unit was scanned
-- label: `count(*)`, `avg(quantity)` by `country` `WHERE removed_at IS NULL`, 0.2% of the rows stamped
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN removed_at int;
UPDATE orders SET removed_at = id WHERE id % 500 = 0;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id, customer_id, country, category, status, amount, quantity, created_at, removed_at)
--@ pg
SELECT country, count(*) AS n, avg(quantity) AS q FROM orders WHERE removed_at IS NULL GROUP BY 1 ORDER BY 1
