-- ORDER BY an unsorted integer column LIMIT n from the sorted run of each unit (order_columns)
-- label: `ORDER BY customer_id LIMIT 100`, `order_columns = 'customer_id'`, no btree on the column
-- ordered: yes
--@ pg
SELECT customer_id FROM orders ORDER BY customer_id LIMIT 100
