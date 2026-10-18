-- numeric columns of the order next to the unnested items: a row level numeric in a predicate and in an expression with an item level integer
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, count(*) AS items, sum(i.qty * o.total) AS weighted, max(o.total - o.discount) AS top_net, count(DISTINCT o.total) AS totals, count(*) FILTER (WHERE o.total > 50.255) AS big
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8, discount float8)
WHERE o.total * 2 >= 100.5 AND i.qty > 1
GROUP BY i.category
ORDER BY i.category
