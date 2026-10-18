-- trap: a jsonpath variable
-- shape: trap
-- expect: plain
-- sets: K4000
-- key: -
SELECT o.lang, count(*) AS orders
FROM orders o
WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL
  AND jsonb_path_exists(o.items, '$[*] ? (@.qty == $q)', '{"q": 3}')
GROUP BY 1
ORDER BY 1
