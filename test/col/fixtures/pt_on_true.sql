-- a join to a one-row sub-select with no condition (ON true): every row of the sub-select is a match
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(*) AS n
FROM orders o JOIN (SELECT kind FROM tenants ORDER BY id LIMIT 1) s ON true
GROUP BY s.kind ORDER BY s.kind
