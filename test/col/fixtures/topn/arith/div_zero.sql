-- division by a column that is zero in some rows: division by zero (22012) like the plan's sort
-- shape: topn
-- expect: agg
-- error: yes
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, grp FROM t_int ORDER BY i8 / (grp - 2), id LIMIT 12
