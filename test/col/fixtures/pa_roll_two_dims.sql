-- preagg: rollup: the groups of two dims of the items (category, kind) with count, sum, count of a float and avg: the tuples of the unit's rollup
-- shape: item-summary-rollup
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, i.kind, count(*) AS items, sum(i.qty) AS qty, count(i.price) AS priced, avg(i.price) AS mean_price
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8)
GROUP BY i.category, i.kind
ORDER BY i.category, i.kind
