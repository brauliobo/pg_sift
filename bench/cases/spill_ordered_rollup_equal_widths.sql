-- the ordered spill of a rollup by its time bucket writes the partial states once, in equal widths of the range of the leaf
-- label: `count(*), sum(amount)` by `date_trunc('hour', created_at)` and `customer_id` (about one group a row), `ORDER BY` the bucket, at `work_mem = 4MB`; `in_memory` runs it at 4GB
-- ordered: yes
-- set: work_mem = '4MB'
--@ pg
SELECT date_trunc('hour', created_at) AS t, customer_id, count(*), sum(amount) FROM orders GROUP BY 1, 2 ORDER BY 1, 2
