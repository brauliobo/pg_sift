-- grouping sets over two arms: an arm that has no kind (NULL) joins the groups of the sets that have the kind; a set without the category
-- sums both arms; a cell is added once per set that covers it
-- shape: item-summary-rollup
-- expect: agg
-- key: -
WITH base AS (
  SELECT o.id AS order_id, i.category AS mc, i.kind AS mk, i.qty, i.price
  FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
  WHERE o.tenant_id < 120 AND o.deleted_at IS NULL)
SELECT category, kind, GROUPING(category, kind) AS g, avg(avg_price) AS value, count(DISTINCT order_id) AS orders, sum(units) AS units, count(*) AS cells
FROM (SELECT order_id, mc AS category, mk AS kind, avg(price) AS avg_price, sum(qty) AS units FROM base WHERE mc IS NOT NULL GROUP BY order_id, mc, mk
      UNION ALL
      SELECT order_id, mc, NULL::text, avg(price), sum(qty) FROM base WHERE mc IS NOT NULL GROUP BY order_id, mc) x
GROUP BY GROUPING SETS ((category, kind), (kind), (category), ()) ORDER BY g, category NULLS FIRST, kind NULLS FIRST
