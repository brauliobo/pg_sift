-- the arms' WHERE and HAVING decide the cells of a row, and a row counts when one cell of the group is left; an outer WHERE on an inner aggregate
-- shape: item-summary-rollup
-- expect: agg
-- key: -
-- scenario: yes
WITH base AS (
  SELECT o.id AS order_id, o.country_code AS cc, i.category AS mc, i.kind AS mk, i.qty, i.price
  FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
  WHERE o.tenant_id < 120 AND o.deleted_at IS NULL)
SELECT category, count(DISTINCT order_id) AS orders, count(*) AS cells, sum(units) AS units
FROM (SELECT order_id, mc AS category, mk AS kind, sum(qty) AS units, avg(price) AS avg_price FROM base WHERE qty > 1 GROUP BY order_id, mc, mk HAVING sum(qty) > 3
      UNION ALL
      SELECT order_id, mc, NULL::text, sum(qty), avg(price) FROM base WHERE qty > 1 GROUP BY order_id, mc HAVING count(*) > 1) x
WHERE avg_price < 150.0123
GROUP BY category ORDER BY category NULLS FIRST
