-- preagg: a pattern on the key: an expression the executor evaluates on the dictionary entries
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT source, count(*) AS n, sum(total) AS revenue
FROM orders
WHERE source LIKE 'market%'
GROUP BY source
ORDER BY source
