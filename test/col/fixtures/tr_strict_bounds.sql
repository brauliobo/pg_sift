-- ctid: strict bounds on both sides, the first and the last offsets of a page
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT count(*) AS n, sum(store_id) AS s FROM orders WHERE ctid > '(20,65535)' AND ctid < '(30,1)'
