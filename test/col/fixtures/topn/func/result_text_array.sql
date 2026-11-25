-- ORDER BY an array of two text columns: the elements compare under the collation of the column
-- shape: topn
-- expect: agg
-- key: -
SELECT id, t, vc FROM t_text ORDER BY ARRAY[t, vc], id LIMIT 12
