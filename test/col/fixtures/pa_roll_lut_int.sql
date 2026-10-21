-- preagg: rollup: a group key that is a band of an integer dim (CASE, coalesce, division), next to a text dim
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
SELECT CASE WHEN i.qty >= 3 THEN 'bulk' WHEN i.qty >= 2 THEN 'pair' ELSE 'single' END AS band, coalesce(i.qty, -1) / 2 AS half, i.kind, count(*) AS items, min(i.price) AS lo, max(i.price) AS hi
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8)
GROUP BY 1, 2, 3
ORDER BY 1, 2, 3
