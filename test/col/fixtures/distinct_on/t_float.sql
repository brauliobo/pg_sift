-- DISTINCT ON over float columns with NaN, infinities and NULLs as sort keys and values
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (grp) grp, id, f4, f8 FROM t_float ORDER BY grp, f8 DESC NULLS LAST, f4, id
