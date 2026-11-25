-- ctid: a lower bound above the upper one
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT count(*) AS n, max(id) AS hi FROM orders WHERE ctid >= '(300,1)' AND ctid <= '(20,1)'
