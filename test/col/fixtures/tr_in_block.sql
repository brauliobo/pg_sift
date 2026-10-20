-- ctid: a range of offsets inside one heap block
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT count(*) AS n, sum(tenant_id) AS s, min(id) AS lo, max(id) AS hi FROM orders WHERE ctid >= '(10,3)' AND ctid < '(10,6)'
