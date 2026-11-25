-- window: a running total and a rank over the aggregate of the same query level
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT source, count(*) AS n, sum(count(*)) OVER (ORDER BY source) AS running, rank() OVER (ORDER BY count(*) DESC, source) AS rnk FROM orders GROUP BY source ORDER BY source
