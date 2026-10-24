-- a report over the keys of a map a row holds: two periods as a spine joined from the left, grouping sets, the buckets of each group as filtered distinct counts of the primary key: the rows are counted with a marker per group, not a set of ids
-- label: `count(DISTINCT id)`, `count(DISTINCT id) FILTER (WHERE value >= 8)` over `jsonb_each(scores)` under `GROUPING SETS` and a spine `LEFT JOIN`
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN scores jsonb;
UPDATE orders SET scores = jsonb_build_object('overall', id % 10 + 1, 'service_staff', (id * 7) % 10 + 1, 'service_speed', (id * 3) % 10 + 1, 'value', (id * 11) % 10 + 1);
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id, customer_id, country, category, status, amount, quantity, created_at, scores) WITH (paths = 'scores.* float8')
--@ pg
WITH periods(period, range) AS (VALUES ('current', tsrange('2025-01-01', '2026-01-01', '[)')), ('previous', tsrange('2024-01-01', '2025-01-01', '[)'))),
     parsed AS (SELECT o.id, o.customer_id, o.created_at, split_part(r.key, '_', 1) AS category, nullif(split_part(r.key, '_', 2), '') AS sub, (r.value #>> '{}')::float8 AS value FROM orders o, jsonb_each(o.scores) r)
SELECT p.period, category, sub, count(DISTINCT id) AS n, count(DISTINCT id) FILTER (WHERE value >= 8) AS promoters, count(DISTINCT id) FILTER (WHERE value <= 6) AS detractors, count(DISTINCT customer_id) AS customers, avg(value) AS value
FROM periods p LEFT JOIN parsed x ON x.created_at <@ p.range
GROUP BY GROUPING SETS ((p.period, category, sub), (p.period, category), (p.period)) ORDER BY 1, 2, 3
