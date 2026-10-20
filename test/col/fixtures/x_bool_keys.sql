-- bool_and and bool_or by a key with groups that only some workers see (rare countries), by two keys and by an expression key
-- shape: flat
-- expect: agg
-- guc: max_parallel_workers_per_gather = 2
-- guc: parallel_setup_cost = 0
-- guc: parallel_tuple_cost = 0
-- guc: min_parallel_table_scan_size = 0
-- key: -
-- scenario: yes
SELECT o.country_code, o.store_id % 3 AS st3, bool_and(o.id % 7 <> 0) AS ba, bool_or(o.id % 997 = 0) AS bo, every(o.total > 0) AS ev, bool_or(o.status IS NULL) AS nulls, count(*) AS n
FROM orders o
GROUP BY o.country_code, o.store_id % 3
ORDER BY 1 NULLS FIRST, 2
