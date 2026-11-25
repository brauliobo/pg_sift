-- preagg: a global aggregate of the rows of one value of a dictionary column
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT count(*) AS n, sum(total) AS revenue, avg(discount) AS mean_discount FROM orders WHERE source = 'web'
