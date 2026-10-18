-- source filters: numeric constants that no value or every value passes
-- shape: quals
-- expect: agg
-- key: -
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- guc: enable_seqscan = off
SELECT (SELECT count(*) FROM orders WHERE total > 1000000) AS above_all,
       (SELECT count(*) FROM orders WHERE total < 1000000) AS below_all,
       (SELECT count(*) FROM orders WHERE total >= 0.001 AND total < 0.0001) AS empty,
       (SELECT count(*) FROM orders WHERE total = 100) AS eq100,
       (SELECT count(*) FROM orders WHERE total <> 100.00 AND store_id IN (1, 2, 3)) AS ne100,
       (SELECT count(*) FROM orders WHERE total BETWEEN 99.995 AND 100.004) AS rounded,
       (SELECT count(*) FROM orders WHERE total > 100 AND placed_at >= DATE '2025-06-01') AS q07
