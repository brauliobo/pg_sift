-- ORDER BY an array of integers with NULL elements, descending
-- shape: topn
-- expect: agg
-- key: -
SELECT id, i4, s2 FROM t_coal ORDER BY ARRAY[i4, s2] DESC NULLS FIRST, id LIMIT 12
