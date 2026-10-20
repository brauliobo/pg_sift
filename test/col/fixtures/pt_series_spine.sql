-- a date spine: a sub-select over generate_series that PostgreSQL keeps a plan (OFFSET 0), joined to a date leaf
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT d.day, count(o.id) AS n, sum(o.total) AS revenue
FROM orders o JOIN (SELECT g::date AS day FROM generate_series(date '2025-03-01', date '2025-03-10', interval '1 day') g OFFSET 0) d ON o.placed_at = d.day
WHERE o.deleted_at IS NULL
GROUP BY d.day ORDER BY d.day
