-- ctid: a range across the first superblock boundary (block 255 / 256)
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT status, count(*) AS n, sum(store_id) AS s, min(id) AS lo, max(id) AS hi FROM orders WHERE ctid > '(250,1)' AND ctid < '(300,4)' GROUP BY status ORDER BY status NULLS FIRST
