-- a top-N that sorts every row that passes: each key past the first is read at every row, and the plan charges it
-- label: `ORDER BY` 6 columns `LIMIT 10`, no usable btree
-- ordered: yes
--@ pg
SELECT customer_id, country, category, status, quantity, amount FROM orders ORDER BY customer_id, country, category, status, quantity, amount LIMIT 10
