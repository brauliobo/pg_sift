-- an expression of a wide leaf
-- shape: expr
-- expect: plain
-- key: -
SELECT count(*) FROM t_wide WHERE n19 + 1 > 5 AND n38 * 2 < 1e6
