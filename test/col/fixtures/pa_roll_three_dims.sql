-- preagg: rollup: three dims (category, kind, quantity) and min and max of a float measure
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, i.kind, i.qty, count(*) AS items, min(i.price) AS cheapest, max(i.price) AS dearest, sum(i.discount) AS discounts
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
GROUP BY i.category, i.kind, i.qty
ORDER BY i.category, i.kind, i.qty
