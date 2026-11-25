-- a key of numeric(38, 10): NaN is a key of its own, NULL another
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
-- guc: enable_sort = off
SELECT n38, count(*) AS n, sum(qty) AS q FROM t_wide GROUP BY n38 ORDER BY n38
