-- coalesce of a float column: NaN sorts last, -0 ties 0
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, f8 FROM t_float ORDER BY coalesce(f8, 0) DESC, id LIMIT 12
