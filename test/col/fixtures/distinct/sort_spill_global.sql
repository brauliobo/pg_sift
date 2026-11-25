-- the same with one group, and a second aggregate that stays in memory
-- shape: distinct
-- expect: agg
-- key: -
-- scenario: yes
-- guc: work_mem = 64
-- guc: hash_mem_multiplier = 1
SELECT count(DISTINCT x) AS dx, count(DISTINCT w) AS dw, count(DISTINCT v) AS dv, count(*) AS n FROM t_dist
