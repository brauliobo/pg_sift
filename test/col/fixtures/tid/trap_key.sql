-- trap: ctid in the select list of an aggregate: min(ctid)
-- shape: tid
-- expect: plain
-- key: -
SELECT min(ctid::text) AS first_tid, count(*) AS n FROM orders WHERE ctid < '(30,1)'
