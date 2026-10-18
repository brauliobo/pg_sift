-- trap: numeric functions other than abs, round, trunc, floor and ceil run in PostgreSQL (generic aggregate)
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, max(power(n2, 2)) AS p, max(sqrt(abs(n2))) AS r FROM t_num GROUP BY grp ORDER BY grp
