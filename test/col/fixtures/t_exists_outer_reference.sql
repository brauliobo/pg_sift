-- trap: an element compared with a column of the outer row
-- shape: trap
-- expect: plain
-- sets: K4000
-- key: -
SELECT o.lang, count(*) AS orders
FROM orders o
WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL
  AND EXISTS (SELECT 1 FROM jsonb_to_recordset(o.items) AS i(qty int) WHERE i.qty = o.store_id % 5)
GROUP BY 1
ORDER BY 1
