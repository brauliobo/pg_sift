-- ctid: global minimum and maximum over a range
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT min(id) AS lo, max(id) AS hi, min(placed_at) AS d0, max(placed_at) AS d1, count(DISTINCT tenant_id) AS tenants FROM orders WHERE ctid >= '(120,3)' AND ctid <= '(480,1)'
