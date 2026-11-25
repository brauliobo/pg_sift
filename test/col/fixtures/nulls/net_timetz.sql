-- timetz keys (12:00+01 and 11:00+00 are told apart by =), extremes, comparisons, min and max
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
SELECT tz, count(*) AS n, count(*) FILTER (WHERE tz = '12:00+01') AS eq, count(*) FILTER (WHERE tz < '12:00+00') AS lt, count(*) FILTER (WHERE tz >= '24:00:00-15:59') AS ge FROM t_net GROUP BY tz ORDER BY tz
