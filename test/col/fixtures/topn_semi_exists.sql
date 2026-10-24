-- an EXISTS with the condition of the sub-select's table, next to a condition of the scan, with an OFFSET
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, store_id, placed_at FROM orders o WHERE EXISTS (SELECT 1 FROM stores s WHERE s.id = o.store_id AND s.country_code IN ('US', 'DE')) AND o.deleted_at IS NULL ORDER BY o.placed_at DESC, o.id LIMIT 10 OFFSET 5
