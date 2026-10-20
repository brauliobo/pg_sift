-- min, max, sum and avg of interval (a count(DISTINCT) makes PostgreSQL sort, which keeps an arbitrary one of equal spellings)
-- no ORDER BY (key: 1 sorts): before PostgreSQL 18 a disabled sort is a cost of 1e10 that the 1% fuzz of the planner ignores, so a sorted GroupAggregate could beat SiftAgg
-- shape: expr
-- expect: agg
-- key: 1
-- guc: enable_sort = off
SELECT grp, min(iv) AS mn, max(iv) AS mx, sum(iv) AS s, avg(iv) AS a FROM t_wide WHERE iv BETWEEN '-1000 years' AND '1000 years' GROUP BY grp
