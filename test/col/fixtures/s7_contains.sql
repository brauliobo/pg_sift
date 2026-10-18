-- S7: the same filter as a jsonb containment, with the facet counts on top
-- shape: S7
-- expect: agg
-- sets: K400 K4000
-- key: -
SELECT o.lang, o.country_code, count(*) AS orders
FROM orders o
WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL AND {range(o.placed_at)}
  AND o.items @> '[{"category": "Electronics", "kind": "express"}]'
GROUP BY 1, 2
ORDER BY 1, 2 NULLS FIRST
