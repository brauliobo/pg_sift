-- EXISTS against a table with duplicate keys: an item counts once however many rows match (a semi join does not replicate), NULL keys match nothing
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, count(*) AS n, count(DISTINCT o.id) AS orders, sum(i.qty) AS units
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
WHERE o.tenant_id < 120 AND o.deleted_at IS NULL AND EXISTS (SELECT 1 FROM map_dup m WHERE m.category = i.category AND m.tag IS NOT NULL)
GROUP BY i.category ORDER BY i.category NULLS FIRST
