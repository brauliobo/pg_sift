-- numeric arithmetic on a numeric(12,2) column: the scale of the result comes from PostgreSQL
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, n2 FROM t_num ORDER BY n2 * 2 NULLS FIRST, id LIMIT 12
