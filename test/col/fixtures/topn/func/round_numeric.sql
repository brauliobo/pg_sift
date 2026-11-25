-- round to one digit, floor, ceil and sign of a numeric
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, n2 FROM t_num ORDER BY round(n2, 1), sign(n2), id LIMIT 12
