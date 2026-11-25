-- a uuid cast to text sorts as text
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, u FROM t_misc ORDER BY u::text COLLATE "C", id LIMIT 12
