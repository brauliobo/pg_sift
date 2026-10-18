-- zone pruning: an equality on the id and a date bound, grouped on a dictionary column
-- shape: quals
-- expect: agg
-- key: -
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- guc: enable_seqscan = off
SELECT lang, count(*) AS n, sum(store_id) AS s
FROM orders
WHERE id = 777 AND placed_at >= DATE '2022-01-01'
GROUP BY lang
ORDER BY lang
