-- S7: the same filter as jsonb_path_exists
-- shape: S7
-- expect: any
-- sets: K400 K4000
-- key: -
SELECT count(*) AS orders, count(DISTINCT o.store_id) AS stores
FROM orders o
WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL AND {range(o.placed_at)}
  AND jsonb_path_exists(o.items, '$[*] ? (@.category == "Electronics" && @.kind == "express")')
