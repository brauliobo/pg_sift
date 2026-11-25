-- power overflows (22003) in the rows of 1e300
-- shape: topn
-- expect: agg
-- error: yes
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, f8 FROM t_float ORDER BY power(f8, 2), id LIMIT 12
