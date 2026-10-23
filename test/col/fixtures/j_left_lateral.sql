-- LEFT JOIN LATERAL over the items ON true: an order with no item is one tuple with NULL item columns (it keeps its group), per order and per group of orders
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.store_id, count(*) AS tuples, count(DISTINCT o.id) AS orders, count(i.qty) AS items, sum(i.qty) AS qty, max(i.category) AS category
FROM orders o LEFT JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8) ON true
WHERE o.deleted_at IS NULL AND o.store_id < 300
GROUP BY o.store_id ORDER BY o.store_id
