-- trap: a join to a one-row sub-select with no condition (ON true) has no key
-- shape: trap
-- expect: plain
-- key: -
SELECT s.kind, count(*) AS n
FROM orders o JOIN (SELECT kind FROM stores ORDER BY id LIMIT 1) s ON true
GROUP BY s.kind ORDER BY s.kind
