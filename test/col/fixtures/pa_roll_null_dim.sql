-- preagg: rollup: the elements with no value in a dim (IS NULL) and the NULL group of a dim
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.kind, count(*) AS items, sum(i.qty) AS qty
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8)
WHERE i.category IS NULL OR i.kind IS NULL OR i.qty > 2
GROUP BY i.kind
ORDER BY i.kind
