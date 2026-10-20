-- bool_and, bool_or and every without a GROUP BY in two workers: one row from the merge of the partial states, NULL inputs ignored
-- shape: flat
-- expect: agg
-- guc: max_parallel_workers_per_gather = 2
-- guc: parallel_setup_cost = 0
-- guc: parallel_tuple_cost = 0
-- guc: min_parallel_table_scan_size = 0
-- key: -
-- scenario: yes
SELECT bool_and(o.total > 10) AS ba, bool_or(o.store_id = 400) AS bo, every(o.lang IS NOT NULL) AS ev, bool_or(o.country_code = 'LU') AS lu,
       bool_and(o.status = 'paid') FILTER (WHERE o.status IS NULL) AS null_only, bool_and(o.discount > 0) AS ba_discount, count(*) AS n
FROM orders o
