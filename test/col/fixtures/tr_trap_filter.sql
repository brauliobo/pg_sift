-- trap: ctid in the FILTER of an aggregate
-- shape: tid
-- expect: plain
-- key: -
SELECT count(*) FILTER (WHERE ctid >= '(10,1)') AS late_rows, count(*) AS n, sum(store_id) AS s FROM orders WHERE store_id < 100
