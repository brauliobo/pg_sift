-- trap: the inner groups are not one order's share (no order id in the group): the average over them is not the average over the orders
-- shape: trap
-- expect: any
-- key: -
-- scenario: yes
SELECT kind, avg(avg_price) AS value, count(*) AS groups
FROM (SELECT i.category, i.kind, avg(i.price) AS avg_price FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8) WHERE o.store_id < 120 AND o.deleted_at IS NULL GROUP BY i.category, i.kind) x
GROUP BY kind ORDER BY kind NULLS FIRST
