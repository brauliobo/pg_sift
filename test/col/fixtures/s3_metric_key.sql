-- a key of the free key metrics at the row level next to the unnest of the items (absent for the orders without it)
-- shape: S3
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, count(*) AS n, count((o.metrics->>'m_total')::float8) AS with_metric, avg((o.metrics->>'m_total')::float8) AS m_total,
       sum(i.qty * (i.price - i.discount)) AS revenue
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
WHERE o.tenant_id < 120 AND o.deleted_at IS NULL
GROUP BY i.category ORDER BY 1
