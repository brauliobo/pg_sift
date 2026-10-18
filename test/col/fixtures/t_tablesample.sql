-- trap: TABLESAMPLE (a sample of the heap, which an index cannot reproduce)
-- shape: trap
-- expect: plain
-- key: -
SELECT count(*) AS n, sum(store_id) AS s FROM orders TABLESAMPLE BERNOULLI (30) REPEATABLE (42)
