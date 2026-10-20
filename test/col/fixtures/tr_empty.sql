-- ctid: an empty range (the bounds meet and cross)
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT count(*) AS n, sum(store_id) AS s, min(id) AS lo FROM orders WHERE ctid > '(5,3)' AND ctid < '(5,3)'
