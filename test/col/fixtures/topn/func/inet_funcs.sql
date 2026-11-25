-- host, family and masklen of an inet column
-- shape: topn
-- expect: agg
-- key: -
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- scenario: yes
SELECT id, ip FROM t_net ORDER BY family(ip), masklen(ip) DESC, host(ip), id LIMIT 12
