-- DISTINCT whose hash set outgrows work_mem moves to a tuplesort and gives the same rows (64 kB: the set of 4000 ids does not fit)
-- shape: flat
-- expect: agg
-- guc: work_mem = 64
-- key: -
SELECT source, cardinality(array_agg(DISTINCT id)) AS n, sum(DISTINCT id % 1000) AS s, string_agg(DISTINCT country_code, ',') AS ccs, array_agg(DISTINCT placed_at ORDER BY placed_at DESC) AS days
FROM orders GROUP BY source ORDER BY source
