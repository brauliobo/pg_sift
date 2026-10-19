-- bigint and int4 expressions PostgreSQL evaluates: a modulo as a group key over the whole int4 range, quotients, a division guarded by a FILTER, count(DISTINCT) of an expression
-- shape: expr
-- expect: agg
-- key: -
SELECT i4 % 7 AS k, count(*) AS n, max(i8 / 3) AS q, min(i8 % 1000) AS m, count(i4 / 2) AS c, max(1000 / i4) FILTER (WHERE i4 <> 0) AS g, count(DISTINCT i8 % 10) AS d
FROM t_int GROUP BY 1 ORDER BY 1 NULLS FIRST
