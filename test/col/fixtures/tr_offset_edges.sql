-- ctid: bounds on the offsets of the first and last block of the range
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT count(*) AS n, sum(tenant_id) AS s, min(id) AS lo, max(id) AS hi FROM orders WHERE ctid >= '(20,4)' AND ctid <= '(30,2)'
