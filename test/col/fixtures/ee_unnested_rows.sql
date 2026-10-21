-- EXISTS over the items beside an unnest of the order's metrics: the test is of the order, its answer is kept for the tuples that repeat it
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT m.key, count(*) AS n, count(DISTINCT o.id) AS orders, avg(m.value::float8) AS mean
FROM orders o JOIN tenants s ON s.id = o.tenant_id CROSS JOIN LATERAL jsonb_each(o.metrics) m
WHERE o.tenant_id < 300 AND o.deleted_at IS NULL AND EXISTS (SELECT 1 FROM jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
    WHERE i.category = CASE s.kind WHEN 'online' THEN 'Toys' ELSE 'Apparel' END)
GROUP BY m.key ORDER BY m.key
