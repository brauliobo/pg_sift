-- jsonpath != on a text member: a JSON null member is not equal to the literal, a missing key is not
-- shape: trap
-- expect: agg
-- sets: K4000
-- key: -
-- scenario: yes
SELECT o.lang, count(*) AS orders
FROM orders o
WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL
  AND jsonb_path_exists(o.items, '$[*] ? (@.kind != "gift")')
GROUP BY 1
ORDER BY 1
