-- the query unnests the notes and tests the items of the order against a column of the order: the owners of the items and of the notes are not the same map
-- shape: S7
-- expect: agg
-- sets: K4000
-- key: -
-- scenario: yes
SELECT n.type, count(*) AS notes, count(DISTINCT o.id) AS orders
FROM orders o, jsonb_to_recordset(o.notes) AS n(type text)
WHERE o.tenant_id IN ({tenants}) AND o.deleted_at IS NULL
  AND EXISTS (SELECT 1 FROM jsonb_to_recordset(o.items) AS i(qty int) WHERE i.qty >= o.tenant_id)
GROUP BY 1
ORDER BY 1
