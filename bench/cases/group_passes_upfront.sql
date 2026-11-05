-- GROUP BY one integer column with a group table several times hash memory: the scan chooses the number of passes from the estimate and reads the table once for each pass
-- label: `count(*)` and `sum(quantity)` `GROUP BY customer_id` under an aggregate of the groups (hash memory 1MB: the groups are several times what it holds)
-- ordered: yes
-- set: work_mem = '1MB'; hash_mem_multiplier = 1
--@ pg
SELECT count(*) AS groups, sum(n) AS orders_total, sum(total) AS quantity_total FROM (SELECT customer_id, count(*) AS n, sum(quantity) AS total FROM orders GROUP BY customer_id) g
