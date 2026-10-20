-- trap: a volatile function in the ARRAY sub-select
-- shape: trap
-- expect: plain
-- key: -
SELECT w.label_a, count(*) AS n
FROM t_work w
WHERE w.dim_a_id = ANY (ARRAY(SELECT e.id FROM t_work_dim e WHERE e.id <= 5 AND random() < 2))
GROUP BY 1
ORDER BY 1 NULLS FIRST
