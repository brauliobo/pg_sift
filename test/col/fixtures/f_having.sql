-- flat: HAVING on aggregates that are not in the select list
-- shape: flat
-- expect: agg
-- key: -
SELECT tenant_id, count(*) AS n, max(placed_at) AS last_day
FROM orders
GROUP BY tenant_id
HAVING count(*) > 8 AND max(placed_at) > DATE '2025-06-01' AND min(tenant_id) > 0
ORDER BY n DESC, tenant_id
