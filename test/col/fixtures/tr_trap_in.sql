-- trap: ctid IN a list
-- shape: tid
-- expect: plain
-- key: -
SELECT count(*) AS n, sum(store_id) AS s FROM orders WHERE ctid = ANY (ARRAY['(1,1)'::tid, '(2,2)', '(300,1)'])
