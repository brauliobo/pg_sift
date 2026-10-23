-- the opening of thousands of partitions priced: the normal plan wins
-- label: `GROUP BY country` over 3200 hash partitions of 62 rows, planning included (the normal plan is cheaper)
--@ setup
CREATE TABLE orders_p (LIKE orders) PARTITION BY HASH (id);
DO $$ BEGIN FOR i IN 0..3199 LOOP EXECUTE format('CREATE TABLE orders_p_%s PARTITION OF orders_p FOR VALUES WITH (MODULUS 3200, REMAINDER %s)', i, i); END LOOP; END $$;
INSERT INTO orders_p SELECT * FROM orders;
CREATE INDEX orders_p_col ON orders_p USING sift_col ({cols});
VACUUM (FREEZE, ANALYZE) orders_p
--@ pg
SELECT country, count(*), sum(amount) FROM orders_p GROUP BY country ORDER BY country
