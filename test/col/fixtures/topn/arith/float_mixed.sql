-- float4 plus float8
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, f4, f8 FROM t_float WHERE f8 < 1e100 ORDER BY f4 + f8, id LIMIT 12
