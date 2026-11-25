-- a key of integer and text arrays: the empty one, NULL elements
-- shape: expr
-- expect: agg
-- key: -
SELECT ia::text AS k, count(*) AS n FROM t_misc GROUP BY ia ORDER BY ia
