-- items-exists-same-element: orders with an item of a category and kind and a price above a bound, the conditions on the same element (EXISTS over the elements)
-- shape: items-exists-same-element
-- expect: agg
-- sets: K10 K400 K4000
-- key: -
-- scenario: yes
SELECT o.lang, count(*) AS orders, count(DISTINCT o.tenant_id) AS tenants
FROM orders o
WHERE o.tenant_id IN ({tenants}) AND o.source IN ({sources}) AND o.deleted_at IS NULL AND {range(o.placed_at)}
  AND EXISTS (SELECT 1 FROM jsonb_to_recordset(o.items) AS i(category text, kind text, price float8)
              WHERE i.category = 'Electronics' AND i.kind = 'express' AND i.price > 50)
GROUP BY 1
ORDER BY 1
