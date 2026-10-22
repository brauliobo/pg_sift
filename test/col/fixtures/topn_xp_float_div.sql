-- float division: division by zero (22012)
-- shape: topn
-- expect: agg
-- error: yes
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, f8 FROM t_float ORDER BY 1 / f8, id LIMIT 12
