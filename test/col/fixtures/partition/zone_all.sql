-- preagg: a comparison the unit zones satisfy as a whole (ids and dates start above the bounds) next to a histogram
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT source, count(*) AS n, sum(total) AS revenue
FROM orders
WHERE id > 0 AND placed_at >= DATE '2000-01-01'
GROUP BY source
ORDER BY source
