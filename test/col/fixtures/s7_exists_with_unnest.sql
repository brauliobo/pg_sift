-- S7: an unnesting query with existential tests on the same array and on another array of the same row
-- shape: S7
-- expect: agg
-- sets: K4000
-- key: -
SELECT i.category, count(*) AS items, count(DISTINCT o.id) AS orders
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, price float8)
WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL AND i.price > 10
  AND o.items @> '[{"kind": "express"}]' AND EXISTS (SELECT 1 FROM jsonb_to_recordset(o.notes) AS n(type text) WHERE n.type = 'kind_a')
GROUP BY 1
ORDER BY 1
