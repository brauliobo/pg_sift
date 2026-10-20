-- strict mode with a positive filter: the same documents match as in lax mode
-- shape: trap
-- expect: agg
-- sets: K4000
-- key: -
-- scenario: yes
SELECT o.lang, count(*) AS orders
FROM orders o
WHERE o.tenant_id IN ({tenants}) AND o.deleted_at IS NULL
  AND jsonb_path_exists(o.items, 'strict $[*] ? (@.qty == 3)')
GROUP BY 1
ORDER BY 1
