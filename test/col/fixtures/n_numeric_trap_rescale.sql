-- trap: a rescale to a column type rounds and may raise an error: PostgreSQL's cast does (generic aggregate)
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, max(n2::numeric(8, 1)) AS rescaled FROM t_num WHERE n2 BETWEEN -50 AND 120 GROUP BY grp ORDER BY grp
