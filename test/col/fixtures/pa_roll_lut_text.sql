-- preagg: rollup: a group key that is a function of a text dim (upper, left), with a test on the dim and a NULL dim
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
SELECT upper(i.category) AS cat, left(i.kind, 2) AS k2, count(*) AS items, sum(i.qty) AS qty, avg(i.price) AS mean_price
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8)
WHERE i.kind <> 'gift' OR i.kind IS NULL
GROUP BY upper(i.category), left(i.kind, 2)
ORDER BY 1, 2
