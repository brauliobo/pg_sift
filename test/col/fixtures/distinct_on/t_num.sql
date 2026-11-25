-- DISTINCT ON over numeric columns of several scales, a numeric sort key
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (grp) grp, id, n2, n0, n6 FROM t_num ORDER BY grp, n2 DESC NULLS LAST, n6 NULLS FIRST, id
