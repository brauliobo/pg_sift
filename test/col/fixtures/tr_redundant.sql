-- ctid: several bounds on each side, the tightest wins
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT count(*) AS n, sum(store_id) AS s FROM orders WHERE ctid >= '(10,1)' AND ctid > '(40,2)' AND ctid <= '(400,1)' AND ctid < '(350,3)'
