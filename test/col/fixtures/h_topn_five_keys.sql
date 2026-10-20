-- trap: five sort keys, above the limit of the top-N scan (four): the normal plan
-- shape: topn
-- expect: plain
-- key: -
SELECT id, store_id, source, status, lang FROM orders WHERE memo @@> 'express' ORDER BY store_id DESC, source, status NULLS FIRST, lang DESC, id LIMIT 20
