-- trap: a product of 54 digits that a floor brings back to 37 does not fit 128 bits: PostgreSQL computes it (generic aggregate)
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, max(floor(n6 * n6 * n6)) AS f FROM t_num GROUP BY grp ORDER BY grp
