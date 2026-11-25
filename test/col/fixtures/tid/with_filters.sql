-- ctid: with value filters on the same scan
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT source, count(*) AS n, sum(tenant_id) AS s FROM orders WHERE ctid >= '(100,1)' AND ctid < '(450,1)' AND tenant_id < 60 AND status <> 'paid' GROUP BY source ORDER BY source
