-- trap: a NaN inside the arithmetic: PostgreSQL computes it (generic aggregate)
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, max(n2 + 'NaN'::numeric) AS nan, min(n2 * 'Infinity'::numeric) AS inf FROM t_num WHERE n2 > 0 GROUP BY grp ORDER BY grp
