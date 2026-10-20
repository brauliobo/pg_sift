-- trap: a range of ctid under an OR
-- shape: tid
-- expect: plain
-- key: -
SELECT count(*) AS n, sum(store_id) AS s FROM orders WHERE ctid < '(10,1)' OR store_id = 3
