-- floor of a numeric descending, ceil as the tie
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, n2 FROM t_num ORDER BY floor(n2) DESC, ceil(n2), id LIMIT 12
