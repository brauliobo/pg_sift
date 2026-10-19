-- trap: a negated filter is unknown for a missing key
-- shape: trap
-- expect: plain
-- sets: K4000
-- key: -
SELECT o.lang, count(*) AS orders
FROM orders o
WHERE o.tenant_id IN ({tenants}) AND o.deleted_at IS NULL
  AND jsonb_path_exists(o.items, '$[*] ? (!(@.kind == "gift"))')
GROUP BY 1
ORDER BY 1
