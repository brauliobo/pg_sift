-- DISTINCT ON over integer columns at the limits of their types, with NULLs in the sort keys
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (grp) grp, id, i4, i8 FROM t_int ORDER BY grp, i4 DESC NULLS LAST, i8 NULLS FIRST, id
