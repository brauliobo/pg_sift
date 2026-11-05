-- GROUP BY two columns whose groups outgrow hash memory: the table writes its groups to partitions and merges them, and the partition of a group is hashed a key at a time
-- label: `count(*)` and `sum(amount)` `GROUP BY customer_id, quantity` under an aggregate of the groups (hash memory 64kB: the estimate says not one share of the groups fits, and the scan spills from the start)
-- ordered: yes
-- set: work_mem = '64kB'; hash_mem_multiplier = 1
--@ pg
SELECT count(*) AS groups, sum(n) AS orders_total, sum(total) AS amount_total FROM (SELECT customer_id, quantity, count(*) AS n, sum(amount) AS total FROM orders GROUP BY customer_id, quantity) g
