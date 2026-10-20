-- a key of ranges (the empty one, NULL) and the range operators
-- shape: expr
-- expect: agg
-- key: -
SELECT r::text AS k, count(*) AS n FROM t_misc GROUP BY r ORDER BY r
