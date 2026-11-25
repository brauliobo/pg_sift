-- a time plus an interval wraps around midnight
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, t FROM t_misc ORDER BY t + interval '5 hours', id LIMIT 12
