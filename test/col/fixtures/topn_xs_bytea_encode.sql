-- the hex text of a bytea column
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, b FROM t_misc ORDER BY encode(b, 'hex') DESC, id LIMIT 12
