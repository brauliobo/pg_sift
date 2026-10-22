-- sum of two numeric columns of different scales, descending
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, n2, n6 FROM t_num ORDER BY n2 + n6 DESC, id LIMIT 12
