-- S7: orders without an item that is discounted or has no kind (NOT EXISTS with OR and IS NULL on the elements; orders without items pass)
-- shape: S7
-- expect: agg
-- sets: K4000
-- key: -
-- scenario: yes
SELECT o.lang, count(*) AS orders
FROM orders o
WHERE o.tenant_id IN ({tenants}) AND o.deleted_at IS NULL
  AND NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(o.items) AS i(kind text, discount float8) WHERE i.discount > 0 OR i.kind IS NULL)
GROUP BY 1
ORDER BY 1
