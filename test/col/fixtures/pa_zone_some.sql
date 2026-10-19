-- preagg: a comparison some units satisfy as a whole and the others scan
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT source, count(*) AS n, sum(total) AS revenue
FROM orders
WHERE placed_at >= DATE '2024-01-01'
GROUP BY source
ORDER BY source
