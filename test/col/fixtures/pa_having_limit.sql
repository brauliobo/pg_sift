-- preagg: HAVING, ORDER BY and LIMIT above the pre-aggregates
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT source, count(*) AS n, sum(total) AS revenue
FROM orders
GROUP BY source
HAVING count(*) > 5
ORDER BY revenue DESC, source
LIMIT 7
