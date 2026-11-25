-- group keys PostgreSQL evaluates: a modulo, a cast to int4 (the rows past its range are filtered), a rounded quotient; NULLs are a group
-- shape: expr
-- expect: agg
-- key: -
SELECT n2 % 3 AS m, n6::int4 AS i, round(n2 / 7, 3) AS q, count(*) AS n, sum(n2) AS s
FROM t_num
WHERE (n6 BETWEEN -1000 AND 1000 OR n6 IS NULL) AND (n2 BETWEEN -50 AND 120 OR n2 IS NULL)
GROUP BY 1, 2, 3
ORDER BY 1 NULLS FIRST, 2 NULLS FIRST, 3 NULLS FIRST
