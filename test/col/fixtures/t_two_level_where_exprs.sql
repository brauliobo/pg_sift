-- the WHERE of the outer query over inner aggregates that is not a comparison with a constant (an OR, two aggregates, an expression of one):
-- PostgreSQL pushes it into the arms' HAVING and evaluates it on the values of each inner group
-- the float thresholds are off the grid of the one-decimal prices: an average on the grid depends on the addition order, which differs between PostgreSQL's own plans
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
WITH base AS (
  SELECT o.id AS order_id, i.category AS mc, i.kind AS mk, i.qty, i.price
  FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
  WHERE o.tenant_id < 120 AND o.deleted_at IS NULL)
SELECT category, kind, avg(avg_price) AS value, count(DISTINCT order_id) AS orders, sum(units) AS units, count(*) AS cells
FROM (SELECT order_id, mc AS category, mk AS kind, avg(price) AS avg_price, sum(qty) AS units, count(*) AS n FROM base WHERE mc IS NOT NULL GROUP BY order_id, mc, mk
      UNION ALL
      SELECT order_id, mc, NULL::text, avg(price), sum(qty), count(*) FROM base WHERE mc IS NOT NULL GROUP BY order_id, mc) x
WHERE (avg_price > 100.0123 OR units < 2) AND units * 2 > 3 AND avg_price < n * 90.0123 AND coalesce(units, 0) + n <> 4
GROUP BY category, kind ORDER BY category, kind NULLS FIRST
