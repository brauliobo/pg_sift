-- trap: an ARRAY sub-select that reads the outer row runs for every row: the plan stays
-- shape: trap
-- expect: plain
-- key: -
SELECT w.mix, count(*) AS n
FROM t_load w
WHERE w.entity_id = ANY (ARRAY(SELECT e.id FROM t_load_entity e WHERE e.id <= w.src_id))
GROUP BY 1
ORDER BY 1 NULLS FIRST
