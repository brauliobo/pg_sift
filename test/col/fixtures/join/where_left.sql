-- a WHERE over the columns of a LEFT JOIN filters its output rows: the NULL row of an item without a match passes `IS NULL` and fails a comparison
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, count(*) AS n, count(m.tag) AS tagged, count(DISTINCT o.id) AS orders
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
LEFT JOIN map_dup m ON m.category = i.category
WHERE o.tenant_id < 120 AND o.deleted_at IS NULL AND (m.tag IS NULL OR m.tag = 'b') AND coalesce(m.category, '') <> 'Shoes'
GROUP BY i.category ORDER BY i.category NULLS FIRST
