-- ctid: offset 0 and the largest offset keep whole blocks
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT count(*) AS n, sum(store_id) AS s FROM orders WHERE ctid >= '(20,0)' AND ctid <= '(30,65535)'
