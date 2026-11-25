-- NaN stored in a numeric column: the index keeps NULL plus the overflow flag and PostgreSQL's own functions evaluate those rows (quotients, a modulo key, a float8 comparison)
-- shape: expr
-- expect: agg
-- key: -
SELECT n2 % 3 AS k, count(*) AS n, sum(n2 / 3) AS q, count(*) FILTER (WHERE n2 > 1.5::float8) AS big, avg(n6 / 2) AS a
FROM t_nan GROUP BY 1 ORDER BY 1 NULLS FIRST
