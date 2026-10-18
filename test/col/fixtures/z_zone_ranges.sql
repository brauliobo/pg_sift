-- zone pruning: bounds on an integer, a second integer and a date column are intersected per leaf, the other conjuncts still apply
-- shape: quals
-- expect: agg
-- key: -
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- guc: enable_seqscan = off
SELECT lang, count(*) AS n, count(deleted_at) AS c, sum(tenant_id) AS s, min(id) AS lo, max(id) AS hi
FROM orders
WHERE id > 300 AND id <= 900 AND id >= 400 AND tenant_id >= 0 AND placed_at < DATE '2025-01-01' AND tenant_id <> 7
GROUP BY lang
ORDER BY lang
