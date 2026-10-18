-- trap: float group keys: -0 and 0 are one group, every NaN is one group, infinities, NULL, float4 and float8
-- shape: trap
-- expect: agg
-- key: -
SELECT f4, f8, count(*) AS n, sum(id) AS ids FROM t_float GROUP BY f4, f8 ORDER BY f4, f8
