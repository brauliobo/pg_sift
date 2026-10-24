-- the largest value of a float key of each row, averaged by group: the inner group by the row id has one cell per row, which the two-level scan folds into the groups without a table of the rows
-- label: `avg(score)` by country of `max((scores ->> 'overall')::float8)` grouped by order id and country (1,000,000 inner groups)
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN metrics_a jsonb;
UPDATE orders SET metrics_a = jsonb_build_object('mhead', id % 10 + 1, 'service_staff', (id * 7) % 10 + 1);
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id, customer_id, country, category, status, amount, quantity, created_at, metrics_a) WITH (paths = 'metrics_a.* float8')
--@ pg
SELECT country, count(id) AS n, round(avg(score)::numeric, 6) AS value FROM (SELECT id, country, max((metrics_a ->> 'mhead')::float8) AS score FROM orders GROUP BY id, country) s GROUP BY country ORDER BY country
