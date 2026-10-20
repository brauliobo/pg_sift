-- min, max, sum and avg of every wide numeric by group, NaN poisoning the groups that have one
-- no ORDER BY (key: 1 sorts): before PostgreSQL 18 a disabled sort is a cost of 1e10 that the 1% fuzz of the planner ignores, so a sorted GroupAggregate could beat SiftAgg
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: 1
-- guc: enable_sort = off
SELECT grp, min(n38) AS mn, max(n38) AS mx, sum(n38) AS s, avg(n38) AS a, sum(n19) AS s19, max(nimg) AS mx_img, sum(nimg) AS s_img FROM t_wide GROUP BY grp
