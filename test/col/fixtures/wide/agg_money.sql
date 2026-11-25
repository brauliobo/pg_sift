-- sum, avg and min of money without the overflow
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
SELECT grp, sum(m) AS s, avg(m::numeric) AS a, min(m) AS mn, max(m) AS mx FROM t_wide WHERE m BETWEEN '-1000000' AND '1000000' GROUP BY grp ORDER BY grp
