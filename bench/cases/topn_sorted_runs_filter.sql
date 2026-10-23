-- a top-N with order_columns reads the sorted run of a unit also when a WHERE leaves few of its rows
-- label: `WHERE status = 'paid' ORDER BY customer_id LIMIT 100` (10% of the rows), `order_columns = 'customer_id'`
-- ordered: yes
--@ pg
SELECT customer_id FROM orders WHERE status = 'paid' ORDER BY customer_id LIMIT 100
