-- trap: IS DISTINCT FROM is not a comparison of the kernels (the normal plan, same rows)
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, count(*) FILTER (WHERE n2 IS DISTINCT FROM 7) AS dist FROM t_num GROUP BY grp ORDER BY grp
