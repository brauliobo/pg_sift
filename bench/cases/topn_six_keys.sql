-- a top-N on 6 sort columns: the heap of the scan holds up to 8 keys, and the keys past the first are compared only for the rows that tie
-- label: `ORDER BY` 6 columns `LIMIT 10`, no usable btree
-- ordered: yes
--@ pg
SELECT customer_id, country, category, status, quantity, amount FROM orders ORDER BY customer_id, country, category, status, quantity, amount LIMIT 10
