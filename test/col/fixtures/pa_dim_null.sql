-- preagg: the rows with no value in the dictionary column
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT count(*) AS n, sum(total) AS revenue FROM orders WHERE status IS NULL
