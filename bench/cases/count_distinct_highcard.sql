-- count(DISTINCT) per group of a hundred thousand groups of a few rows: the sets start as hash sets and the scan is priced as such
-- label: `count(DISTINCT order_date)` by `customer_id` and `category` (about 300,000 groups of 3 rows) at `work_mem = 4GB`
-- set: work_mem = '4GB'; hash_mem_multiplier = 2
--@ pg
SELECT customer_id, category, count(DISTINCT order_date) FROM orders GROUP BY 1, 2
