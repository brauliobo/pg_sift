-- a key of money, which has no hash opclass
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
-- guc: enable_sort = off
SELECT m, count(*) AS n, min(qty) AS q FROM t_wide GROUP BY m ORDER BY m
