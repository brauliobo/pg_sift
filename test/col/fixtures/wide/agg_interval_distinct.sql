-- count(DISTINCT interval) per group: 1 month, 30 days and 720 hours are one value
-- shape: expr
-- expect: agg
-- key: -
SELECT grp, count(DISTINCT iv) AS d FROM t_wide WHERE iv BETWEEN '-1000 years' AND '1000 years' GROUP BY grp ORDER BY grp
