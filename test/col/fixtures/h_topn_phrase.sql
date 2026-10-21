-- a phrase: the rows the index leaves undecided are rechecked on their text before they reach the heap of the best
-- shape: topn
-- expect: any
-- key: -
-- scenario: yes
SELECT id, placed_at FROM orders WHERE memo @@> '"cable wire"' ORDER BY placed_at DESC, id DESC LIMIT 10
