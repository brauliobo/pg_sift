-- preagg: rollup: a subset of the dims of the unit (kind alone, then kind and quantity) re-aggregated from the tuples
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.kind, count(*) AS items, sum(i.qty) AS qty, min(i.qty) AS least, max(i.qty) AS most
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8)
GROUP BY i.kind
ORDER BY i.kind
