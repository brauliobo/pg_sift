-- nine cell keys beyond the id (the masks hold 32): the outer aggregate runs in the scan
-- shape: item-summary-rollup
-- expect: agg
-- key: -
-- scenario: yes
WITH base AS (
  SELECT o.id AS order_id, o.source AS src, o.lang AS lng, o.status AS st, o.country_code AS cc, o.tenant_id AS sid, i.category AS mc, i.kind AS mk, i.qty, o.customer_id AS cu
  FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
  WHERE o.tenant_id < 120 AND o.deleted_at IS NULL)
SELECT count(DISTINCT order_id) AS orders, count(*) AS cells, sum(units) AS units
FROM (SELECT order_id, src, lng, st, cc, sid, mc, mk, qty, cu, sum(qty) AS units FROM base GROUP BY order_id, src, lng, st, cc, sid, mc, mk, qty, cu
      UNION ALL
      SELECT order_id, src, lng, st, cc, sid, mc, NULL::text, NULL::int, cu, sum(qty) FROM base GROUP BY order_id, src, lng, st, cc, sid, mc, cu) x
