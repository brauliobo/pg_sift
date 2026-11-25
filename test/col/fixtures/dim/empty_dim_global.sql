-- an empty dimension without GROUP BY: one row, zero counts
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT count(*) AS rows, count(o.id) AS orders, sum(o.total) AS revenue
FROM map_empty m LEFT JOIN orders o ON o.source = m.category
