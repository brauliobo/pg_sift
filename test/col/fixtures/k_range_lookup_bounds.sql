-- the ranges as two bounds of the lookup row: lower() and upper() of the range (an empty or an open range has a NULL bound)
-- shape: flat
-- expect: agg
-- key: 1
-- scenario: yes
SELECT p.label, count(*) AS n, sum(o.total) AS total
FROM orders o JOIN periods p ON o.placed_at >= lower(p.range) AND o.placed_at < upper(p.range)
GROUP BY p.label ORDER BY 1 NULLS FIRST
