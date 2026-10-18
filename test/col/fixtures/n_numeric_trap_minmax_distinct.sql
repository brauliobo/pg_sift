-- trap: min and max of DISTINCT values, and an ordered-set aggregate over numerics, keep the normal plan (same rows)
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, min(DISTINCT n2) AS lo, max(DISTINCT n2) AS hi FROM t_num GROUP BY grp ORDER BY grp
