-- a wide conjunct under OR
-- shape: expr
-- expect: agg
-- key: -
SELECT count(*) FROM t_wide WHERE n38 > 1234.5 OR grp = 3
