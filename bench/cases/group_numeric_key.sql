-- GROUP BY a numeric column (and an int8 one) from the histogram of the cube
-- label: `GROUP BY` a `numeric(3,0)` column of 5 values, `count`, `sum`
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN q8 bigint, ADD COLUMN qn numeric(3,0);
UPDATE orders SET q8 = quantity, qn = quantity;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col ({cols}, q8, qn)
--@ pg
SELECT qn, count(*), sum(amount) FROM orders GROUP BY qn ORDER BY qn
