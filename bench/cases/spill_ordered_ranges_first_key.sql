-- a scan whose groups the ORDER BY wants in order spills in ranges of the first key and keeps the order, where it spilled by hash and a Sort followed
-- label: `count(*), sum(amount)` by `date_trunc('hour', created_at)`, `customer_id` and `status` of the last year of orders, `ORDER BY` the bucket descending, at `work_mem = 4MB`; `in_memory` runs it at 4GB
-- ordered: yes
-- set: work_mem = '4MB'
--@ pg
SELECT date_trunc('hour', created_at) AS t, customer_id, status, count(*), sum(amount) FROM orders WHERE created_at >= timestamp '2025-01-01' GROUP BY 1, 2, 3 ORDER BY 1 DESC, 2, 3
