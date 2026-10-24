-- ln of a float raises (22014) for a value that is not positive
-- shape: topn
-- expect: agg
-- error: yes
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, f8 FROM t_float ORDER BY ln(f8), id LIMIT 12
