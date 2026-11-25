-- an empty dimension: no group (and no row of the aggregate without GROUP BY beyond its one row of zeros)
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT m.tag, count(o.id) AS orders, count(*) AS rows
FROM map_empty m LEFT JOIN orders o ON o.source = m.category
GROUP BY m.tag ORDER BY m.tag NULLS FIRST
