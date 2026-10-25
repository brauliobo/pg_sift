-- The `orders` table of the benchmark cases (bench/cases.tsv, dataset orders) for the other engines of the generic suites: the rows of bench/sql/store/gen.sql (steps/generic.py runs that file
-- first) with the two derived columns the cases use, as steps/cases.py makes them, and the btrees of the PostgreSQL lane of the store benchmark.
CREATE TABLE orders AS SELECT *, (customer_id % 6313)::int AS segment_id, created_at::date AS order_date FROM orders_src ORDER BY id;
DROP TABLE orders_src;
ALTER TABLE orders ADD PRIMARY KEY (id);
CREATE INDEX orders_created ON orders (created_at);
CREATE INDEX orders_country_status ON orders (country, status);
VACUUM (FREEZE, ANALYZE) orders;
