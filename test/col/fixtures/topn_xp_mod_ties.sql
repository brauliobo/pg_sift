-- a remainder has few values: the ties fall to the next key
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, i4 FROM t_coal WHERE i4 BETWEEN -500 AND 500 ORDER BY i4 % 7 DESC NULLS LAST, id LIMIT 25
