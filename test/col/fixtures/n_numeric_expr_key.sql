-- numeric expressions as group keys: rounding and truncation (also to a negative scale), a cast to an integer, floor, products; the key prints with PostgreSQL's scale
-- shape: S6
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT round(n2, 1) AS r1, floor(n6)::int8 AS f6, trunc(n2, -1) AS t_m1, (n2 * 2)::int8 AS d2, count(*) AS n, sum(n2) AS s
FROM t_num
WHERE n2 BETWEEN -50 AND 120
GROUP BY 1, 2, 3, 4
ORDER BY 1 NULLS FIRST, 2, 3, 4
