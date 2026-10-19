-- the cast of a numeric to an integer when the rows with a NaN are left out by the query
-- shape: expr
-- expect: agg
-- key: -
SELECT grp, sum(n2::int) AS s, max(n6::bigint) AS b FROM t_nan WHERE n2 <> 'NaN' AND n6 <> 'NaN' GROUP BY grp ORDER BY grp
