-- 5,000 hash partitions of about 6 rows each: the scan registers its snapshot once and finds a partition by bisection
-- label: `GROUP BY country` over 5,000 hash partitions of 6 rows, planning included
--@ setup
CREATE TABLE orders_p (LIKE orders) PARTITION BY HASH (id);
DO $$ BEGIN FOR i IN 0..4999 LOOP EXECUTE format('CREATE TABLE orders_p_%s PARTITION OF orders_p FOR VALUES WITH (MODULUS 5000, REMAINDER %s)', i, i); END LOOP; END $$;
INSERT INTO orders_p SELECT * FROM orders ORDER BY id LIMIT 30000;
CREATE INDEX orders_p_col ON orders_p USING sift_col ({cols});
VACUUM (FREEZE, ANALYZE) orders_p
--@ pg
SELECT country, count(*), sum(amount) FROM orders_p GROUP BY country ORDER BY country
