-- DISTINCT ON the primary key: every group is one row
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (id) id, store_id, total, status FROM orders WHERE store_id < 10 ORDER BY id
