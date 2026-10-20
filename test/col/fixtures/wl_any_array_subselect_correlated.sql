-- trap: an ARRAY sub-select that reads the outer row runs for every row: the plan stays
-- shape: trap
-- expect: plain
-- key: -
SELECT w.label_a, count(*) AS n
FROM t_work w
WHERE w.dim_a_id = ANY (ARRAY(SELECT e.id FROM t_work_dim e WHERE e.id <= w.lookup_id))
GROUP BY 1
ORDER BY 1 NULLS FIRST
