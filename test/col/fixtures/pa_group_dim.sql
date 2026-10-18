-- preagg: count, sum and count of a numeric per dictionary column: the groups of the unit histograms
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT source, count(*) AS n, sum(total) AS revenue, count(total) AS priced, avg(discount) AS mean_discount
FROM orders
GROUP BY source
ORDER BY source
