-- S7: orders per language with an item of a category and kind and a price above a bound, the three conditions on the same element
-- sets: K10 K400 K4000
-- key: 1
--@ pg
SELECT o.lang, count(*) AS orders
FROM orders o
WHERE o.tenant_id IN ({tenants}) AND o.source IN ({sources}) AND o.deleted_at IS NULL AND {range(o.placed_at)}
  AND EXISTS (SELECT 1 FROM jsonb_to_recordset(o.items) AS i(category text, kind text, price float8)
              WHERE i.category = 'Electronics' AND i.kind = 'express' AND i.price > 200)
GROUP BY 1
--@ flat
SELECT i.lang, count(DISTINCT i.order_id) AS orders
FROM pg_items i
WHERE i.tenant_id IN ({tenants}) AND i.source IN ({sources}) AND i.deleted_at IS NULL AND {range(i.placed_at)}
  AND i.category = 'Electronics' AND i.kind = 'express' AND i.price > 200
GROUP BY 1
--@ ch
SELECT i.lang AS lang, count(DISTINCT i.order_id) AS orders
FROM items_tuned i
WHERE i.tenant_id IN ({tenants}) AND i.source IN ({sources}) AND i.deleted = 0 AND {range(i.placed_at)}
  AND i.category = 'Electronics' AND i.kind = 'express' AND i.price > 200
GROUP BY lang
