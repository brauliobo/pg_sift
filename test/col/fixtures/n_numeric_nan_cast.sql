-- NaN stored in a numeric column behind a cast to an integer: PostgreSQL raises for the NaN of a row it takes, and for no other
-- shape: expr
-- expect: agg
-- key: -
-- error: either
SELECT grp, sum(n2::int) AS s FROM t_nan GROUP BY grp ORDER BY grp
