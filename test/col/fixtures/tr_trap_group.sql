-- trap: GROUP BY ctid
-- shape: tid
-- expect: plain
-- key: -
SELECT ctid::text AS t, count(*) AS n FROM orders WHERE ctid < '(3,1)' GROUP BY ctid ORDER BY ctid
