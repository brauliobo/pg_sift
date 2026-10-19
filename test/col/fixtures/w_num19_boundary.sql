-- numeric(19, 2): 19 digits, the first precision that is not one 64-bit lane
-- shape: trap
-- expect: agg
-- key: -
SELECT grp, count(*) AS n, sum(qty) AS q FROM t_wide WHERE n19 >= 9223372036854775.81 AND n19 <> 18446744073709551.62 GROUP BY grp ORDER BY grp
