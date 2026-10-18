-- a false gate above one arm: no inner group, no outer group (and a query without GROUP BY still has its row)
-- shape: S2
-- expect: agg
-- key: -
SELECT count(*) AS cells, sum(units) AS units, avg(avg_price) AS value
FROM (SELECT o.id AS order_id, i.category, avg(i.price) AS avg_price, sum(i.qty) AS units
      FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
      WHERE o.tenant_id < 120 AND o.deleted_at IS NULL GROUP BY o.id, i.category) x
WHERE now() < '2020-01-01'::timestamptz
