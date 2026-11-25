-- group keys over several leaves: of the order, and of the order and of its items
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT coalesce(o.attributes->>'channel', o.source) || '/' || o.country_code AS channel_cc, CASE WHEN o.source = 'web' THEN i.category ELSE coalesce(i.kind, '?') END AS pick,
       count(*) AS n, sum(i.qty * i.price) AS revenue
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
WHERE o.tenant_id < 120 AND o.deleted_at IS NULL
GROUP BY 1, 2 ORDER BY 1 NULLS FIRST, 2
