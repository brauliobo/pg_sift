-- float8 arithmetic: NaN and the infinities sort last, -0 ties 0
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, f8 FROM t_float WHERE f8 < 1e100 ORDER BY f8 * 2 DESC, id LIMIT 12
