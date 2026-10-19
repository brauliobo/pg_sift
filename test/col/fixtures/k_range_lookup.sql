-- a date placed in the ranges of a small table (a date in several, in none; an empty, an open and a NULL range, a NULL label)
-- shape: flat
-- expect: agg
-- key: 1
-- scenario: yes
SELECT p.label, count(*) AS n, sum(o.total) AS total, count(DISTINCT o.store_id) AS stores
FROM orders o JOIN periods p ON o.placed_at <@ p.range
WHERE o.deleted_at IS NULL
GROUP BY p.label ORDER BY 1 NULLS FIRST
