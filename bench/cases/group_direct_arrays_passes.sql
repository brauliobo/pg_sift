-- GROUP BY two integer columns whose direct arrays (the capacities of the keys multiply) are more than hash memory: the groups are in the hash table at once and the scan is read in a few passes
-- label: `count(*)` and `sum(amount)` `GROUP BY customer_id, quantity` under an aggregate of the groups (hash memory 2MB; the product of the capacities of the keys is more than it holds)
-- ordered: yes
-- set: work_mem = '2MB'; hash_mem_multiplier = 1
--@ pg
SELECT count(*) AS groups, sum(n) AS orders_total, sum(total) AS amount_total FROM (SELECT customer_id, quantity, count(*) AS n, sum(amount) AS total FROM orders GROUP BY customer_id, quantity) g
