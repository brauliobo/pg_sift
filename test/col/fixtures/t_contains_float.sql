-- trap: containment of a float member compares numerics exactly
-- shape: trap
-- expect: plain
-- sets: K4000
-- key: -
SELECT o.lang, count(*) AS orders
FROM orders o
WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL
  AND o.items @> '[{"price": 5.5}]'
GROUP BY 1
ORDER BY 1
