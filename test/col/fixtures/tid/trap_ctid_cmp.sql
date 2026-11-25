-- trap: ctid compared with a column
-- shape: tid
-- expect: plain
-- key: -
SELECT count(*) AS n FROM orders WHERE ctid >= ('(' || (id % 50) || ',1)')::tid
