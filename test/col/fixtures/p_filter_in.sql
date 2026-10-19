-- source filters: short IN and NOT IN lists of an integer column, and one longer than the source takes
-- shape: quals
-- expect: agg
-- key: -
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- guc: enable_seqscan = off
SELECT country_code, count(*) AS n, sum(total) AS t, max(placed_at) AS last,
       count(*) FILTER (WHERE tenant_id IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12)) AS long_in
FROM orders
WHERE tenant_id IN (1, 2, 3, 4, 5, 6, 40, 120, 399) AND tenant_id NOT IN (3, 4) AND total < 450
GROUP BY country_code
ORDER BY country_code NULLS FIRST
