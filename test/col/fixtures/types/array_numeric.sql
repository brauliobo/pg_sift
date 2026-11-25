-- an array of numerics whose elements are equal but spelled differently: only the groups are counted
-- shape: expr
-- expect: agg
-- key: -
SELECT count(*) AS groups, sum(n) AS rows FROM (SELECT na, count(*) AS n FROM t_misc GROUP BY na) g
