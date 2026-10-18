-- ORDER BY a date column, newest first, over a text match: the heap is read for the 10 rows only
-- shape: topn
-- expect: any
-- key: -
-- scenario: yes
SELECT id, placed_at, memo FROM orders WHERE memo @@> 'damaged OR broken' ORDER BY placed_at DESC, id DESC LIMIT 10
