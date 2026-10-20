-- ctid: NOT of a range is the opposite range (PostgreSQL turns it into one)
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT count(*) AS n, sum(store_id) AS s FROM orders WHERE NOT (ctid >= '(300,1)')
