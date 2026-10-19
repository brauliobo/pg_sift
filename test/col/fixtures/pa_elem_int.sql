-- preagg: the items' quantity as the key, a float measure and a test of the key
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.qty, count(*) AS items, avg(i.price) AS mean_price
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(qty int, price float8)
WHERE i.qty > 1
GROUP BY i.qty
ORDER BY i.qty
