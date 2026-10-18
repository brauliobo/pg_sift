-- trap: aggregates of the columns of a lookup join (a key with two rows, NULL values, categories without a row: NULL columns of the LEFT JOIN)
-- shape: trap
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, count(m.label) AS c, count(DISTINCT m.label) AS d, min(m.label) AS lo, max(m.label) AS hi, sum(m.weight) AS sw, avg(m.weight) AS aw,
       sum(m.factor) AS sf, avg(m.factor) AS af, min(m.rank) AS mr, max(m.since) AS ms, sum(m.weight * i.qty) AS wq, count(*) AS n
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
LEFT JOIN map_vals m ON m.category = i.category
WHERE o.tenant_id < 120 AND o.deleted_at IS NULL
GROUP BY i.category ORDER BY i.category
