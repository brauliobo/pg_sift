-- numeric to integer rounds; out of range raises
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, n2 FROM t_num WHERE n2 BETWEEN -1000 AND 1000 ORDER BY n2::int, id LIMIT 12
