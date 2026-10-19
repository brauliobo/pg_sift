-- trap: aggregates of mapped columns over no row, and over rows none of which has a mapped value
-- shape: trap
-- expect: agg
-- key: -
SELECT count(m.label) AS c, count(DISTINCT m.label) AS d, min(m.label) AS lo, sum(m.weight) AS sw, avg(m.weight) AS aw, sum(m.factor) AS sf, max(m.since) AS ms
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
LEFT JOIN map_vals m ON m.category = i.category
WHERE o.store_id < 120 AND o.deleted_at IS NULL AND i.category IN ('Auto', 'Baby')
