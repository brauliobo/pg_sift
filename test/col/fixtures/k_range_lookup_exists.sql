-- the dates in a range of the table (EXISTS) and in none (NOT EXISTS)
-- shape: flat
-- expect: agg
-- key: 1
-- scenario: yes
SELECT o.status, count(*) AS n, sum(o.total) AS total
FROM orders o
WHERE EXISTS (SELECT 1 FROM periods p WHERE o.placed_at <@ p.range AND p.label = 'cur')
  AND NOT EXISTS (SELECT 1 FROM periods p WHERE o.placed_at <@ p.range AND p.label = 'prev')
GROUP BY o.status ORDER BY 1 NULLS FIRST
