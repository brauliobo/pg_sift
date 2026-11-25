-- a range on a numeric of two lanes: the pair of lanes decides, a NaN row is greater than every number
-- shape: trap
-- expect: agg
-- key: -
SELECT grp, count(*) AS n, sum(qty) AS q FROM t_wide WHERE n38 > 1234.5 AND n38 <= 12345678901234567890.123456789 GROUP BY grp ORDER BY grp
