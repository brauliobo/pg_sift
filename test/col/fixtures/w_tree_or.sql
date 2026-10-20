-- a tree of wide conjuncts: OR, NOT, AND
-- shape: expr
-- expect: agg
-- key: -
SELECT grp, count(*) AS n FROM t_wide WHERE (n38 > 1e3 OR iv > interval '1 month') AND NOT (m < '0' AND nimg IS NULL) GROUP BY grp ORDER BY grp
