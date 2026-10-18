-- numeric expressions in aggregate arguments: + - * and unary minus with the scales PostgreSQL gives (2 + 6, 2 * 6, 2 * 0), abs, an integer column cast to numeric, negative values, NULL operands
-- shape: S6
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(n2 * n6) AS c, sum(n2 * n2) AS s_mul, avg(n2 + n6) AS a_add, sum(n2 - n6) AS s_sub, min(n2 * grp) AS lo_int, max(-n2) AS hi_neg, sum(abs(n2)) AS s_abs,
       sum(n2 * 2) AS s_dbl, sum(n2 + 1.5) AS s_const, min(n0 * 0.10) AS lo_scale2, max(n0 + n2) AS hi_mix, max((n2 + 1) * (n6 - 1)) AS hi_nested, sum((n2 + 1) * 2 - n2) AS s_nested
FROM t_num
GROUP BY grp
ORDER BY grp
