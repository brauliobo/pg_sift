-- ctid: a lower bound past the end of the table
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT count(*) AS n, sum(store_id) AS s FROM orders WHERE ctid >= '(100000,1)'
