-- OFFSET with an expression key
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, s2 FROM t_coal WHERE s2 BETWEEN -100 AND 100 ORDER BY s2 + 1, id LIMIT 7 OFFSET 20
