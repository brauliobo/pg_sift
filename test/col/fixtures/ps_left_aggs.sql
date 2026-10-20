-- the aggregates and keys of every kind over the NULL leaves of the dimension rows no order matched: dates, numerics, floats, text, jsonb paths, DISTINCT, generic
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, date_trunc('year', o.placed_at) AS yr, o.attributes->>'channel' AS channel, count(*) AS rows, count(o.id) AS orders, count(DISTINCT o.customer_id) AS customers,
	   sum(o.total) AS total, avg(o.total) AS mean, min(o.placed_at) AS first_day, max(o.status) AS last_status, bool_or(o.total > 100) AS big,
	   sum(o.discount) AS discount, array_agg(DISTINCT o.country_code ORDER BY o.country_code) AS countries, sum((o.metrics->>'cpu')::float8) AS cpu
FROM tenants s LEFT JOIN orders o ON o.tenant_id = s.id AND o.deleted_at IS NULL
WHERE s.id < 120
GROUP BY s.kind, date_trunc('year', o.placed_at), o.attributes->>'channel' ORDER BY 1, 2 NULLS FIRST, 3 NULLS FIRST
