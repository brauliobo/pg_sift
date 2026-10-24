-- a report over the keys of a map a row holds, joined from the left to a spine of two periods under grouping sets: the cast map value reaches the
-- scan as a placeholder, which is read as the float leaf, so the average is the leaf aggregate and the buckets compare the decoded values
-- label: `avg(value)`, `count(*) FILTER (WHERE value >= 8)` of `(r.value #>> '{}')::float8` over `jsonb_each(scores)`, a spine `LEFT JOIN` and `GROUPING SETS`
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN metrics_a jsonb;
UPDATE orders SET metrics_a = jsonb_build_object('mhead', id % 10 + 1, 'service_staff', (id * 7) % 10 + 1, 'service_speed', (id * 3) % 10 + 1, 'value', (id * 11) % 10 + 1);
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id, customer_id, country, category, status, amount, quantity, created_at, metrics_a) WITH (paths = 'metrics_a.* float8')
--@ pg
WITH periods(period, range) AS (VALUES ('current', tsrange('2025-01-01', '2026-01-01', '[)')), ('previous', tsrange('2024-01-01', '2025-01-01', '[)'))),
     parsed AS (SELECT o.created_at, split_part(r.key, '_', 1) AS category, nullif(split_part(r.key, '_', 2), '') AS sub, (r.value #>> '{}')::float8 AS value FROM orders o, jsonb_each(o.metrics_a) r)
SELECT p.period, category, sub, count(*) AS n, count(*) FILTER (WHERE value >= 8) AS promoters, count(*) FILTER (WHERE value <= 6) AS detractors, round(avg(value)::numeric, 6) AS value
FROM periods p LEFT JOIN parsed x ON x.created_at <@ p.range
GROUP BY GROUPING SETS ((p.period, category, sub), (p.period, category), (p.period)) ORDER BY 1, 2, 3
