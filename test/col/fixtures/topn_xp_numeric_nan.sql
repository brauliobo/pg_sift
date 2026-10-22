-- NaN sorts above every number
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, n2 FROM t_nan ORDER BY n2 + 1 DESC, id LIMIT 12
