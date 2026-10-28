-- whole-distinct-orders: whole table, count distinct of the order id
-- sets: -
-- order: 4
-- key:
--@ pg
SELECT count(DISTINCT o.id) AS orders
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(qty int)
--@ flat
SELECT count(DISTINCT i.order_id) AS orders FROM pg_items i
--@ ch
SELECT count(DISTINCT i.order_id) AS orders FROM items_tuned i
