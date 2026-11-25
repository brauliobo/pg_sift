-- trap: min and max of DISTINCT numerics: PostgreSQL's transition functions over the distinct values (generic aggregate)
-- shape: trap
-- expect: agg
-- key: -
SELECT grp, min(DISTINCT n2) AS lo, max(DISTINCT n2) AS hi FROM t_num GROUP BY grp ORDER BY grp
