-- a cross join to a table with a condition of its own, and an outer join with no condition that keeps every order
-- shape: join
-- expect: agg
-- key: -
SELECT t.kind, count(o.id) AS n, count(t.id) AS tenants
FROM orders o LEFT JOIN (SELECT id, kind FROM tenants WHERE id <= 3) t ON true
GROUP BY t.kind ORDER BY t.kind NULLS FIRST
