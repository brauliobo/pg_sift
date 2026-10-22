-- min and max of an integer measure grouped by a dimension of the cube
-- label: `min(customer_id), max(customer_id) GROUP BY category`
-- ordered: yes
--@ pg
SELECT category, min(customer_id), max(customer_id) FROM orders GROUP BY category ORDER BY category
