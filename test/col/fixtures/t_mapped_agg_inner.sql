-- trap: aggregates of the columns of an INNER lookup join, grouped by a mapped column and by nothing; a sum of an int stays in the normal plan
-- shape: trap
-- expect: agg
-- key: -
SELECT m.label, count(*) AS n, count(m.rank) AS c, max(m.weight) AS hw, sum(m.weight) AS sw, avg(m.factor) AS af, count(DISTINCT m.since) AS ds
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
JOIN map_vals m ON m.category = i.category
WHERE o.tenant_id < 120 AND o.deleted_at IS NULL
GROUP BY ROLLUP (m.label) ORDER BY m.label NULLS FIRST
