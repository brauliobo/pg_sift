-- count(DISTINCT) per group with a group table several times hash memory: the scan is read in passes, each for some of the groups, where the spill wrote every set and merged it
-- label: `count(DISTINCT category)`, `count(*)` and `sum(amount)` by `customer_id` (200,000 groups at 1,000,000 orders) at `work_mem = 8MB`
-- ordered: yes
-- set: work_mem = '8MB'; hash_mem_multiplier = 1
--@ pg
SELECT customer_id, count(DISTINCT category), count(*), sum(amount) FROM orders GROUP BY 1 ORDER BY 2 DESC, 1 LIMIT 100
