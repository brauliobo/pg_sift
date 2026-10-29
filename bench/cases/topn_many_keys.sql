-- 5 sort columns, past the 4 the heap held before: the top-N scan keeps them all
-- label: `ORDER BY` 5 columns `LIMIT 10`, no usable btree
-- ordered: yes
--@ pg
SELECT customer_id, country, category, status, quantity FROM orders ORDER BY customer_id, country, category, status, quantity LIMIT 10
