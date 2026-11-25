-- BM25: the count of the matches of an OR query with a filter on a column
-- shape: bm25
-- expect: any
-- key: -
SELECT count(*) AS n, avg(tenant_id) AS avg_store FROM orders WHERE memo @@> 'damaged OR broken' AND placed_at >= DATE '2024-01-01'
