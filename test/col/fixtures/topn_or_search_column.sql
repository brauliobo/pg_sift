-- a search ORed with a predicate on a column keeps the normal plan
-- shape: topn_or
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, store_id, placed_at FROM orders WHERE memo @@> 'cable' OR store_id = 3 ORDER BY placed_at DESC, id DESC LIMIT 10
