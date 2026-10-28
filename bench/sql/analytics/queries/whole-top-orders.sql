-- whole-top-orders: whole table, group by the order id (one group per order), top 10
-- sets: -
-- order: 8
-- key: 1
--@ pg
SELECT o.id, count(*) AS c
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(qty int)
GROUP BY 1 ORDER BY c DESC, 1 LIMIT 10
--@ flat
SELECT i.order_id, count(*) AS c FROM pg_items i GROUP BY 1 ORDER BY c DESC, 1 LIMIT 10
--@ ch
SELECT i.order_id AS order_id, count(*) AS c FROM items_tuned i GROUP BY order_id ORDER BY c DESC, order_id LIMIT 10
