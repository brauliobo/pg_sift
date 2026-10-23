-- the same top-N when the column has a btree: the loss to register
-- label: `ORDER BY customer_id LIMIT 1000`, `order_columns = 'customer_id'`, a btree on the column
-- ordered: yes
--@ setup
CREATE INDEX orders_customer ON orders (customer_id)
--@ pg
SELECT customer_id FROM orders ORDER BY customer_id LIMIT 1000
