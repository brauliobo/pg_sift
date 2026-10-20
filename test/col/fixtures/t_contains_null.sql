-- containment of a JSON null member: the index tells it from an absent key
-- shape: trap
-- expect: agg
-- sets: K4000
-- key: -
-- scenario: yes
SELECT o.lang, count(*) AS orders
FROM orders o
WHERE o.tenant_id IN ({tenants}) AND o.deleted_at IS NULL
  AND o.items @> '[{"kind": null}]'
GROUP BY 1
ORDER BY 1
