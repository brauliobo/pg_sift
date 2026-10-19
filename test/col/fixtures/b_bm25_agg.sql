-- BM25 and an aggregate in one query: the full-text match is a filter of the aggregation
-- shape: bm25
-- expect: any
-- key: -
-- scenario: yes
SELECT source, count(*) AS n, count(DISTINCT tenant_id) AS tenants, min(placed_at) AS first_day
FROM orders
WHERE memo @@> 'clean wire' AND deleted_at IS NULL
GROUP BY source
ORDER BY source
