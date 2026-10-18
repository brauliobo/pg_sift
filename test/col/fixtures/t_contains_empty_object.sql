-- trap: containment of {} is true for any object element
-- shape: trap
-- expect: plain
-- sets: K4000
-- key: -
SELECT o.lang, count(*) AS orders
FROM orders o
WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL
  AND o.items @> '[{}]'
GROUP BY 1
ORDER BY 1
