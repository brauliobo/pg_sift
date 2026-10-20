-- min, max, sum and avg of every wide numeric by group, NaN poisoning the groups that have one
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
-- guc: enable_sort = off
SELECT grp, min(n38) AS mn, max(n38) AS mx, sum(n38) AS s, avg(n38) AS a, sum(n19) AS s19, max(nimg) AS mx_img, sum(nimg) AS s_img FROM t_wide GROUP BY grp ORDER BY grp
