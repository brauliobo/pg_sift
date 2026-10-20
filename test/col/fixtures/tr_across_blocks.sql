-- ctid: a range across many blocks of one superblock, grouped
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT source, count(*) AS n, sum(store_id) AS s FROM orders WHERE ctid >= '(40,2)' AND ctid <= '(200,5)' GROUP BY source ORDER BY source
