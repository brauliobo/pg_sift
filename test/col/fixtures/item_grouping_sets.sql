-- item-summary-rollup: the rollup levels as GROUPING SETS over the expanded items
-- shape: item-summary-rollup
-- expect: any
-- sets: K400
-- key: -
SELECT i.category, i.kind, grouping(i.category, i.kind) AS lvl, count(*) AS items, count(DISTINCT o.id) AS orders, avg(i.price) AS avg_price, sum(i.qty) AS units
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8)
WHERE o.tenant_id IN ({tenants}) AND o.deleted_at IS NULL AND {range(o.placed_at)}
GROUP BY GROUPING SETS ((i.category, i.kind), (i.category), ())
ORDER BY 3, 1 NULLS FIRST, 2 NULLS FIRST
