-- a key of time, with the midnight and 24:00:00 edges; min and max
-- shape: expr
-- expect: agg
-- key: -
SELECT t, count(*) AS n, sum(qty) AS q FROM t_misc GROUP BY t ORDER BY t
