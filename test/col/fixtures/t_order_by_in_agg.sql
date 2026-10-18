-- ORDER BY inside aggregates (string_agg, array_agg): a tuplesort per group, then PostgreSQL's transition function
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT source, string_agg(country_code, ',' ORDER BY country_code, id) AS ccs, array_agg(id ORDER BY store_id, id) AS ids
FROM orders WHERE store_id < 4 GROUP BY source ORDER BY source
