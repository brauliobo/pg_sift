-- EXISTS over the items with a lookup per item by the kind of the store: the mapped category of an item, its own where the map has no row (or a NULL one)
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(*) AS n, count(DISTINCT o.id) AS orders, sum(o.total) AS total
FROM orders o JOIN tenants s ON s.id = o.tenant_id
WHERE o.tenant_id < 300 AND o.deleted_at IS NULL AND EXISTS (SELECT 1 FROM LATERAL (
    SELECT coalesce(m.derived_category, i.category) AS category
    FROM jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
    LEFT JOIN LATERAL (SELECT im.derived_category FROM item_map im WHERE im.store_kind = s.kind AND im.category = i.category AND im.kind = i.kind
      AND im.derived_category IS NOT NULL LIMIT 1) m ON true
    WHERE i.qty IS NOT NULL) x WHERE x.category IN ('fashion', 'Home', 'play'))
GROUP BY s.kind ORDER BY s.kind NULLS FIRST
