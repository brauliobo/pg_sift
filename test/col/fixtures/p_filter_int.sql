-- source filters: comparisons of an integer, a date and a scaled numeric (constants of another scale, outside the range), a sparse result with aggregates of other columns
-- shape: quals
-- expect: agg
-- key: -
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- guc: enable_seqscan = off
SELECT country_code, count(*) AS n, count(status) AS c, sum(total) AS t, avg(discount) AS d, min(id) AS lo, max(placed_at) AS last
FROM orders
WHERE total > 100.005 AND total <= 480.5 AND store_id > 7 AND store_id <> 40 AND placed_at >= DATE '2024-06-01' AND discount < 1000.123
GROUP BY country_code
ORDER BY country_code NULLS FIRST
