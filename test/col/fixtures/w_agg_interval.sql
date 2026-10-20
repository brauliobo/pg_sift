-- min, max, sum and avg of interval (a count(DISTINCT) makes PostgreSQL sort, which keeps an arbitrary one of equal spellings)
-- shape: expr
-- expect: agg
-- key: -
-- guc: enable_sort = off
SELECT grp, min(iv) AS mn, max(iv) AS mx, sum(iv) AS s, avg(iv) AS a FROM t_wide WHERE iv BETWEEN '-1000 years' AND '1000 years' GROUP BY grp ORDER BY grp
