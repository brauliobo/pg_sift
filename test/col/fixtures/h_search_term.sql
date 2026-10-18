-- a term of the BM25 index is the row selection of the aggregation (match set from the index, columns from the column index)
-- shape: search
-- expect: any
-- key: -
-- scenario: yes
SELECT source, count(*) AS n, sum(tenant_id) AS s, min(placed_at) AS first_day, max(id) AS last_id
FROM orders WHERE memo @@> 'damaged' AND status <> 'cancelled'
GROUP BY source ORDER BY source
