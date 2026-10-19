-- count(DISTINCT) of values 2 billion apart: no bitmap covers them, a hash set of pairs does
-- shape: distinct
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(DISTINCT w) AS d, count(DISTINCT w) FILTER (WHERE v > 0) AS pos FROM t_dist GROUP BY grp ORDER BY grp
