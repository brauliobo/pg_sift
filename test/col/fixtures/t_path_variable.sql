-- a jsonpath variable that is a constant of the call
-- shape: trap
-- expect: agg
-- sets: K4000
-- key: -
-- scenario: yes
SELECT o.lang, count(*) AS orders
FROM orders o
WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL
  AND jsonb_path_exists(o.items, '$[*] ? (@.qty == $q)', '{"q": 3}')
GROUP BY 1
ORDER BY 1
