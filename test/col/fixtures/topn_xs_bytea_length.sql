-- length and a substring of a bytea column
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, b FROM t_misc ORDER BY length(b), substring(b from 1 for 2), id LIMIT 12
