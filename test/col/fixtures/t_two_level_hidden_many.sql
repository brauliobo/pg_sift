-- trap: more cell keys than the scan has (six group expressions beyond the id): the outer aggregate stays in the normal plan (the arms may run in the scan, which is as fast or faster, so the tag is any)
-- shape: trap
-- expect: any
-- key: -
-- scenario: yes
WITH base AS (
  SELECT o.id AS order_id, o.source AS src, o.lang AS lng, o.status AS st, i.category AS mc, i.kind AS mk, i.qty
  FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
  WHERE o.tenant_id < 120 AND o.deleted_at IS NULL)
SELECT count(DISTINCT order_id) AS orders, count(*) AS cells, sum(units) AS units
FROM (SELECT order_id, src, lng, st, mc, mk, qty, sum(qty) AS units FROM base GROUP BY order_id, src, lng, st, mc, mk, qty
      UNION ALL
      SELECT order_id, src, lng, st, mc, NULL::text, NULL::int, sum(qty) FROM base GROUP BY order_id, src, lng, st, mc) x
