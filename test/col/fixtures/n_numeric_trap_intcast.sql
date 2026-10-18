-- trap: a numeric cast to int4 may raise an out of range error, only a digit bound proves it does not (the normal plan, same rows)
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT n6::int4 AS k, count(*) AS n FROM t_num WHERE n6 BETWEEN -1000 AND 1000 GROUP BY 1 ORDER BY 1 NULLS FIRST
