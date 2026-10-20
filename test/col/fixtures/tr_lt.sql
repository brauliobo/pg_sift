-- ctid: an upper bound only
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT count(*) AS n, sum(store_id) AS s, max(id) AS hi FROM orders WHERE ctid < '(300,2)'
