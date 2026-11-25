-- numeric aggregates without a group, over everything and over no row
-- shape: notes-path-exists
-- expect: agg
-- key: -
SELECT sum(total) AS s, avg(total) AS a, min(total) AS lo, max(total) AS hi, sum(discount) AS sd, sum(total) FILTER (WHERE id < 0) AS none
FROM orders
