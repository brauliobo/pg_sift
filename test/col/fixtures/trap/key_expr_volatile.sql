-- trap: a volatile ON condition is evaluated for each pair of rows by the normal plan: the plan stays
-- shape: trap
-- expect: plain
-- key: -
SELECT cm.derived_category, count(*) AS n
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
JOIN categories_map cm ON cm.category = lower(i.category) AND random() < 2
WHERE o.tenant_id < 120 AND o.deleted_at IS NULL
GROUP BY cm.derived_category ORDER BY 1
