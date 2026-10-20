-- ctid: BETWEEN
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT count(*) AS n, sum(store_id) AS s FROM orders WHERE ctid BETWEEN '(10,1)' AND '(300,2)'
