-- the groups an ORDER BY wants in order are ranked by the value of an integer, date or timestamp key and sorted by their packed ranks, and the sum of a numeric is built from its digits
-- label: `count(*), sum(amount)` by `date_trunc('hour', created_at)` and `customer_id` (about one group a row), `ORDER BY` both, at `work_mem = 4GB`
-- ordered: yes
-- set: work_mem = '4GB'
--@ pg
SELECT date_trunc('hour', created_at) AS t, customer_id, count(*), sum(amount) FROM orders GROUP BY 1, 2 ORDER BY 1, 2
