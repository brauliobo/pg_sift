-- trap: elements of two arrays are joined (the same pair, not the same element)
-- shape: trap
-- expect: plain
-- sets: K4000
-- key: -
SELECT o.lang, count(*) AS orders
FROM orders o
WHERE o.tenant_id IN ({tenants}) AND o.deleted_at IS NULL
  AND EXISTS (SELECT 1 FROM jsonb_to_recordset(o.items) AS i(qty int), jsonb_to_recordset(o.notes) AS n(type text) WHERE i.qty > 3 AND n.type = 'reply')
GROUP BY 1
ORDER BY 1
