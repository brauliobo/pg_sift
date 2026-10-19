-- trap: containment of a JSON null member: the index holds a NULL for an absent key too
-- shape: trap
-- expect: plain
-- sets: K4000
-- key: -
SELECT o.lang, count(*) AS orders
FROM orders o
WHERE o.tenant_id IN ({tenants}) AND o.deleted_at IS NULL
  AND o.items @> '[{"kind": null}]'
GROUP BY 1
ORDER BY 1
