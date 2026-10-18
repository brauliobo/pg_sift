-- trap: numeric division: the scale and rounding of numeric_div, evaluated by PostgreSQL itself (generic aggregate)
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, sum(n2 / 3) AS q FROM t_num GROUP BY grp ORDER BY grp
