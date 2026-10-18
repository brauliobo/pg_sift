-- trap: a product of 48 digits does not fit 128 bits: PostgreSQL computes it (generic aggregate)
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, max(n2 * n2 * n2 * n2) AS wide FROM t_num GROUP BY grp ORDER BY grp
