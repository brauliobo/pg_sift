-- ln of the positive floats
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, f8 FROM t_float WHERE f8 > 0 AND f8 < 'Infinity' ORDER BY ln(f8) DESC, id LIMIT 12
