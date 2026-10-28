-- preagg: count, sum and count of the items' integer and float per category: the groups of the unit's element cubes
-- shape: item-summary-rollup
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, count(*) AS items, sum(i.qty) AS qty, count(i.price) AS priced, avg(i.price) AS mean_price, sum(i.discount) AS discounts
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8, discount float8)
GROUP BY i.category
ORDER BY i.category
