-- square root of the absolute value of a float: NaN, infinity and -0
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, f8 FROM t_float ORDER BY sqrt(abs(f8)), id LIMIT 12
