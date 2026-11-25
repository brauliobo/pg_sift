-- an expression of a wide leaf, exact in PostgreSQL's own operators
-- shape: expr
-- expect: agg
-- key: -
SELECT count(*) FROM t_wide WHERE n19 + 1 > 5 AND n38 * 2 < 1e6
