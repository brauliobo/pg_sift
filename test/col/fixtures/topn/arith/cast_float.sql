-- integer to float8 and back to integer (rounds half to even)
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, f8 FROM t_float WHERE f8 BETWEEN -1000 AND 1000 ORDER BY f8::int DESC, id LIMIT 12
