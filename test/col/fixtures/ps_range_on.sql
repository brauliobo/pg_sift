-- a join with no equality (a range of dates of the dimension) has no lookup key: every dimension row is a candidate of every tuple
-- shape: join
-- expect: any
-- key: -
-- scenario: yes
SELECT p.label, count(o.id) AS orders, count(*) AS rows
FROM periods p LEFT JOIN orders o ON o.placed_at <@ p.range
GROUP BY p.label ORDER BY p.label NULLS FIRST
