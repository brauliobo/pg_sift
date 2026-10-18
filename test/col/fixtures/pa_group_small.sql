-- preagg: a dictionary column of two letters and NULLs, sum of a numeric
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT lang, count(*) AS n, sum(total) AS revenue FROM orders GROUP BY lang ORDER BY lang
