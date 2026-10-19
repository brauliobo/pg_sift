-- preagg: rollup: filters on the dims: an OR of an equality and an IN, a not equal and a LIKE, grouped by a third dim
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, count(*) AS items, sum(i.qty) AS qty, avg(i.price) AS mean_price
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8)
WHERE (i.kind = 'gift' OR i.kind IN ('standard', 'express')) AND i.category <> 'Home' AND i.category LIKE '%o%' AND i.qty IS NOT NULL
GROUP BY i.category
ORDER BY i.category
