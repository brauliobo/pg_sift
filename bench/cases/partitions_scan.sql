-- the scan of a partitioned table: the planner prices the opening of the partitions
-- label: `GROUP BY country` over 400 hash partitions of about 2,500 rows, planning included
--@ setup
CREATE TABLE orders_p (LIKE orders) PARTITION BY HASH (id);
DO $$ BEGIN FOR i IN 0..399 LOOP EXECUTE format('CREATE TABLE orders_p_%s PARTITION OF orders_p FOR VALUES WITH (MODULUS 400, REMAINDER %s)', i, i); END LOOP; END $$;
INSERT INTO orders_p SELECT * FROM orders;
CREATE INDEX orders_p_col ON orders_p USING sift_col ({cols});
VACUUM (FREEZE, ANALYZE) orders_p
--@ pg
SELECT country, count(*), sum(amount) FROM orders_p GROUP BY country ORDER BY country
