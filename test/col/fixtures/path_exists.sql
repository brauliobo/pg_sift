-- items-exists-same-element: the same filter as jsonb_path_exists
-- shape: items-exists-same-element
-- expect: agg
-- sets: K400 K4000
-- key: -
SELECT count(*) AS orders, count(DISTINCT o.tenant_id) AS tenants
FROM orders o
WHERE o.tenant_id IN ({tenants}) AND o.deleted_at IS NULL AND {range(o.placed_at)}
  AND jsonb_path_exists(o.items, '$[*] ? (@.category == "Electronics" && @.kind == "express")')
