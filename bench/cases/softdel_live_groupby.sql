-- a report over the rows that are not soft-deleted: the units with no deleted row are answered from their pre-aggregates, the others are read
-- label: `count(*), sum(amount) WHERE deleted_at IS NULL GROUP BY country`, 1% of the orders soft-deleted in one batch
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN deleted_at timestamp;
UPDATE orders SET deleted_at = created_at + interval '1 day' WHERE id BETWEEN 400000 AND 410000;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col ({cols}, deleted_at)
--@ pg
SELECT country, count(*) AS n, sum(amount) AS revenue FROM orders WHERE deleted_at IS NULL GROUP BY country ORDER BY country
