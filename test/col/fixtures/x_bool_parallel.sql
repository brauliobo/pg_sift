-- bool_and, bool_or and every in two workers: all true, all false, mixed, NULL inputs, FILTER, a group in one worker only, an empty group and a global form
-- shape: flat
-- expect: agg
-- guc: max_parallel_workers_per_gather = 2
-- guc: parallel_setup_cost = 0
-- guc: parallel_tuple_cost = 0
-- guc: min_parallel_table_scan_size = 0
-- key: -
-- scenario: yes
SELECT o.source, bool_and(o.store_id > 3) AS ba, bool_or(o.total > 400) AS bo, every(o.status IS NOT NULL) AS ev,
       bool_and(o.store_id < 0) AS all_false, bool_or(o.store_id < 0) AS none_true, bool_and(o.store_id > 0) AS all_true, bool_or(o.status = 'refunded') AS has_null_input,
       bool_and(o.total > 100) FILTER (WHERE o.store_id < 200) AS ba_filter, bool_or(o.discount > 50) FILTER (WHERE o.id < 0) AS bo_empty,
       bool_and(o.status = 'paid') FILTER (WHERE o.status IS NULL) AS ba_null_only, count(*) AS n
FROM orders o
GROUP BY o.source
ORDER BY o.source
