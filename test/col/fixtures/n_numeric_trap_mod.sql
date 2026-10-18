-- trap: a modulo of a numeric key (the normal plan, same rows)
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT n2 % 3 AS m, count(*) AS n FROM t_num GROUP BY 1 ORDER BY 1 NULLS FIRST
