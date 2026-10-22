-- ORDER BY coalesce(column, constant) LIMIT n
-- label: `ORDER BY coalesce(customer_id, 0) DESC LIMIT 10`
-- ordered: yes
--@ pg
SELECT customer_id FROM orders ORDER BY coalesce(customer_id, 0) DESC LIMIT 10
