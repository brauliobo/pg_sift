-- GROUP BY two integer columns whose key product is far above the direct arrays (500 x 20,000 slots, 20,000 groups of 10 rows): the groups are in the hash table, the outer aggregate reads them
-- label: `count(*)` and `sum(amount)` `GROUP BY g1, g2` (g1 = id % 500, g2 = id % 20000: 20,000 groups) under an aggregate of the groups; run it with CASES_WORK_MEM=4GB
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN g1 int, ADD COLUMN g2 int;
UPDATE orders SET g1 = id % 500, g2 = id % 20000;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id sift.col_int4_ops, g1 sift.col_int4_ops, g2 sift.col_int4_ops, amount)
--@ pg
SELECT count(*) AS groups, sum(n) AS orders_total, sum(total) AS amount_total FROM (SELECT g1, g2, count(*) AS n, sum(amount) AS total FROM orders GROUP BY g1, g2) g
