-- the soft-deleted rows: the units whose rows are all NULL in the column (the live part of the table) are not read
-- label: `count(*), sum(amount) WHERE deleted_at IS NOT NULL GROUP BY status`, 1% of the orders soft-deleted in one batch (the rows of one id range)
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN deleted_at timestamp;
UPDATE orders SET deleted_at = created_at + interval '1 day' WHERE id BETWEEN 400000 AND 410000;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col ({cols}, deleted_at)
--@ pg
SELECT status, count(*) AS n, sum(amount) AS revenue FROM orders WHERE deleted_at IS NOT NULL GROUP BY status ORDER BY status
