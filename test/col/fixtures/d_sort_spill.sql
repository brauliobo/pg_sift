-- hash_mem of 64 kB: the bitmaps of x (1 million values by 8 groups) do not fit, nor does the set of its pairs, so they go through a sort
-- shape: distinct
-- expect: agg
-- key: -
-- scenario: yes
-- guc: work_mem = 64
-- guc: hash_mem_multiplier = 1
SELECT grp, count(DISTINCT x) AS dx, count(DISTINCT w) AS dw, count(DISTINCT v) AS dv FROM t_dist GROUP BY grp ORDER BY grp
