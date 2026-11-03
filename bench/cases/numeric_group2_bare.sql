-- GROUP BY two columns (16 groups) with count, sum and avg of a numeric column without a precision: the scan against the plan PostgreSQL keeps
-- label: `count(*)`, `sum(x)` and `avg(x)` of a bare `numeric` column `GROUP BY g1, g2` (4 x 4 = 16 groups); run it with CASES_WORK_MEM=4GB and CASES_WORKERS = the workers of the page
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN g1 int, ADD COLUMN g2 int, ADD COLUMN x numeric;
UPDATE orders SET g1 = id % 4, g2 = (id / 4) % 4, x = amount;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id sift.col_int4_ops, g1 sift.col_int4_ops, g2 sift.col_int4_ops, x)
--@ pg
SELECT g1, g2, count(*) AS n, sum(x) AS total, avg(x) AS mean FROM orders GROUP BY g1, g2 ORDER BY g1, g2
