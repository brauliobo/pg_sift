-- a text sort key of a search is a top-N too
-- shape: topn
-- expect: agg
-- key: -
SELECT id, status FROM orders WHERE memo @@> 'gift' ORDER BY status, id LIMIT 10
