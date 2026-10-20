-- bool_and and bool_or of no row: NULL, in two workers, grouped and global
-- shape: flat
-- expect: agg
-- guc: max_parallel_workers_per_gather = 2
-- guc: parallel_setup_cost = 0
-- guc: parallel_tuple_cost = 0
-- guc: min_parallel_table_scan_size = 0
-- key: -
SELECT bool_and(o.store_id > 3) AS ba, bool_or(o.total > 400) AS bo, every(o.status IS NOT NULL) AS ev, count(*) AS n
FROM orders o
WHERE o.store_id < 0
