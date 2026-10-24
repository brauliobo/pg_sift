-- more than 4 sort columns: the planner prices the sort and keeps the normal plan while that is cheaper
-- label: `ORDER BY` 5 columns `LIMIT 10`, no usable btree
-- ordered: yes
--@ pg
SELECT customer_id, country, category, status, quantity FROM orders ORDER BY customer_id, country, category, status, quantity LIMIT 10
