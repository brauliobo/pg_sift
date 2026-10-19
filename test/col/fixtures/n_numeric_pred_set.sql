-- numeric IN, NOT IN (NULL, repeated and inexact members, none representable), BETWEEN, IS NULL, AND/OR of numeric comparisons in a WHERE
-- shape: S6
-- expect: agg
-- key: -
SELECT grp, count(*) AS n,
       count(*) FILTER (WHERE n2 IN (10.5, 7, 0.05, 1.234)) AS in_set,
       count(*) FILTER (WHERE n2 NOT IN (10.5, 7, 0.05, 1.234)) AS not_in_set,
       count(*) FILTER (WHERE n2 IN (10.5, NULL)) AS in_null,
       count(*) FILTER (WHERE n2 NOT IN (10.5, NULL)) AS not_in_null,
       count(*) FILTER (WHERE n2 IN (1.234, 5.678)) AS in_none,
       count(*) FILTER (WHERE n2 NOT IN (1.234, 5.678)) AS not_in_none,
       count(*) FILTER (WHERE n0 IN (1, 2, 3, 999999999999999999, 1e30)) AS in_n0,
       count(*) FILTER (WHERE n2 BETWEEN 0.05 AND 10.5) AS between_,
       count(*) FILTER (WHERE n2 BETWEEN 0.051 AND 10.499) AS between_inexact,
       count(*) FILTER (WHERE n2 NOT BETWEEN 0.05 AND 10.5) AS not_between,
       count(*) FILTER (WHERE n2 BETWEEN SYMMETRIC 10.5 AND -3.25) AS between_sym,
       count(*) FILTER (WHERE n2 IS NULL) AS is_null,
       count(*) FILTER (WHERE n6 IS NOT NULL) AS not_null,
       count(*) FILTER (WHERE n2 > 0 AND n6 < 1 OR n0 = 3) AS and_or,
       count(*) FILTER (WHERE 0.05 < n2) AS commuted
FROM t_num
WHERE n2 IS NOT NULL OR grp = 2
GROUP BY grp ORDER BY grp
