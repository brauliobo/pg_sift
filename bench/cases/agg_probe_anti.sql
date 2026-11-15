-- NOT EXISTS with a condition on the child that its index does not hold: the child's rows are loaded once instead of a probe of its table for each order
-- label: `SELECT count(*) FROM pa_orders o WHERE NOT EXISTS (SELECT 1 FROM pa_items i WHERE i.order_id = o.id AND i.price > 150)` over 200,000 orders and 600,000 items
-- ordered: no
--@ setup
CREATE TABLE pa_orders AS SELECT g AS id, 1 + g % 997 AS customer_id, timestamptz '2025-01-01' + g * interval '7 minutes' AS created_at FROM generate_series(1, {rows}) g;
CREATE TABLE pa_items AS SELECT g AS id, 1 + (g * 7) % {rows} AS order_id, 1 + g % 4 AS qty, ((g * 37) % 20000) / 100.0 AS price FROM generate_series(1, {rows} * 3) g;
ALTER TABLE pa_orders ADD PRIMARY KEY (id);
ALTER TABLE pa_items ADD PRIMARY KEY (id);
CREATE INDEX pa_items_order ON pa_items (order_id);
CREATE INDEX pa_orders_col ON pa_orders USING sift_col (id, customer_id, created_at);
CREATE INDEX pa_items_col ON pa_items USING sift_col (id, order_id, qty, price);
VACUUM ANALYZE pa_orders, pa_items
--@ pg
SELECT count(*) FROM pa_orders o WHERE NOT EXISTS (SELECT 1 FROM pa_items i WHERE i.order_id = o.id AND i.price > 150)
