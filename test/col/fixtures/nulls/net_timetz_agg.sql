-- timetz min, max, count distinct, IN, and the time of day of the value
-- shape: expr
-- expect: agg
-- key: -
SELECT grp, min(tz) AS lo, max(tz) AS hi, count(DISTINCT tz) AS d, count(*) FILTER (WHERE tz IN ('12:00+01', '04:05:06+00')) AS in_, max(tz::time) AS t, max(extract(hour FROM tz)) AS h FROM t_net GROUP BY grp ORDER BY grp
