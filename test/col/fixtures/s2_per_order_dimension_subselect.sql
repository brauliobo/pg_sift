-- an outer key that is a CASE over a sub-select (source IN (SELECT .. FROM dim_source), which holds a source twice and lacks some): every arm plans a sub-plan of its own for it
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
WITH base AS (
  SELECT o.id AS order_id, i.category AS mc, i.kind AS mk, i.qty, i.price,
         (CASE WHEN o.source IN (SELECT source FROM dim_source) THEN jsonb_build_object('id', o.source, 'type', 'source') ELSE NULL END) AS dimension
  FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
  WHERE o.tenant_id < 120 AND o.deleted_at IS NULL)
SELECT category, dimension, count(DISTINCT order_id) AS orders, count(*) AS cells, sum(units) AS units, avg(avg_price) AS value
FROM (SELECT order_id, mc AS category, dimension, avg(price) AS avg_price, sum(qty) AS units FROM base GROUP BY order_id, mc, dimension
      UNION ALL
      SELECT order_id, NULL::text, dimension, avg(price), sum(qty) FROM base GROUP BY order_id, dimension) x
GROUP BY category, dimension ORDER BY category NULLS FIRST, dimension::text NULLS FIRST
