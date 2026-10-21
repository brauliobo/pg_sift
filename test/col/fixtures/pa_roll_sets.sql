-- preagg: rollup: grouping sets (ROLLUP, CUBE, an empty set) over the dims, each set folding the tuples under its own columns
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, i.kind, count(*) AS items, sum(i.qty) AS qty, avg(i.price) AS mean_price
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8)
WHERE i.kind IS DISTINCT FROM 'gift'
GROUP BY GROUPING SETS ((i.category, i.kind), (i.category), (i.kind), ())
ORDER BY i.category NULLS LAST, i.kind NULLS LAST, items, qty
