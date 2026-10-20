-- trap: ctid of the joined table, not of the scanned one
-- shape: tid
-- expect: plain
-- key: -
SELECT count(*) AS n, sum(o.total) AS revenue FROM orders o JOIN stores s ON s.id = o.store_id WHERE s.ctid >= '(0,2)' AND o.store_id < 100
