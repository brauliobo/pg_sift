-- the divisor is never zero in the rows the WHERE keeps
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, grp, i8 FROM t_int WHERE grp <> 2 ORDER BY i8 / (grp - 2), id LIMIT 12
