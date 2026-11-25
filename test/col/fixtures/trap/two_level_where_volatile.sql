-- trap: a volatile condition of the outer query is evaluated once per inner group by the normal plan: the outer aggregate stays in the normal plan (the inner one may still run in the scan)
-- shape: trap
-- expect: any
-- key: -
SELECT category, avg(avg_price) AS value, count(DISTINCT order_id) AS orders
FROM (SELECT o.id AS order_id, i.category, avg(i.price) AS avg_price
      FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
      WHERE o.tenant_id < 120 AND o.deleted_at IS NULL GROUP BY o.id, i.category) x
WHERE random() < 2
GROUP BY category ORDER BY category
