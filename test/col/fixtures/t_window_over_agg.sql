-- trap: a window function over the aggregate
-- shape: trap
-- expect: any
-- key: -
SELECT source, count(*) AS n, sum(count(*)) OVER (ORDER BY source) AS running, rank() OVER (ORDER BY count(*) DESC, source) AS rnk FROM orders GROUP BY source ORDER BY source
