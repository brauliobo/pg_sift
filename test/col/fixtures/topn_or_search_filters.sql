-- an OR of searches with column filters
-- shape: topn_or
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, store_id, placed_at FROM orders WHERE (memo @@> 'cable' OR memo @@> 'wire') AND store_id < 300 AND source <> 'web' ORDER BY placed_at, id LIMIT 8 OFFSET 3
