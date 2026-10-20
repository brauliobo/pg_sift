-- trap: a number against a float8 leaf: jsonpath compares the exact numeric and the leaf holds the double nearest to it, so != and == can differ
-- shape: trap
-- expect: plain
-- sets: K4000
-- key: -
SELECT o.lang, count(*) AS orders,
       count(*) FILTER (WHERE jsonb_path_exists(o.items, '$[*] ? (@.price != 100)')) AS price_ne
FROM orders o
WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL
GROUP BY 1
ORDER BY 1
