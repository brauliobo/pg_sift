-- count(DISTINCT) per group with a group table that spills: the sets of all the partitions are written in one walk
-- label: `count(DISTINCT quantity)` by `customer_id` (40,000 groups) at `work_mem = 1MB`
-- ordered: yes
-- set: work_mem = '1MB'; hash_mem_multiplier = 1
--@ pg
SELECT customer_id, count(DISTINCT quantity), count(DISTINCT status), sum(amount) FROM orders GROUP BY 1 ORDER BY 1 LIMIT 1000
