-- a wide conjunct under OR reads the value
-- shape: expr
-- expect: plain
-- key: -
SELECT count(*) FROM t_wide WHERE n38 > 1234.5 OR grp = 3
