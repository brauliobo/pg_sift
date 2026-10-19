-- a LEFT JOIN to the ranges: the dates in none of them are kept with NULL columns, a date in several is counted for each
-- shape: flat
-- expect: agg
-- key: 1
-- scenario: yes
SELECT p.label, count(*) AS n, count(p.range) AS placed
FROM orders o LEFT JOIN periods p ON o.placed_at <@ p.range AND p.label IN ('cur', 'prev', 'open')
GROUP BY p.label ORDER BY 1 NULLS FIRST
