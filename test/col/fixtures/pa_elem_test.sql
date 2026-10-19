-- preagg: a test of the items' kind and a zone comparison on the order: the cube groups that pass, the units whose zone lies inside
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.kind, count(*) AS items, sum(i.qty) AS qty
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(kind text, qty int)
WHERE o.id >= 1 AND i.kind IN ('a', 'b', 'c', 'standard', 'gift')
GROUP BY i.kind
ORDER BY i.kind
