-- trap: jsonpath != is true for a JSON null member, which the index cannot tell from an absent key
-- shape: trap
-- expect: plain
-- sets: K4000
-- key: -
SELECT o.lang, count(*) AS orders
FROM orders o
WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL
  AND jsonb_path_exists(o.items, '$[*] ? (@.kind != "gift")')
GROUP BY 1
ORDER BY 1
