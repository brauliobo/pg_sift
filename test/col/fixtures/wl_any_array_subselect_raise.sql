-- trap: the sub-select divides by zero for one entity: the normal plan runs it when a row reaches the test (none does: the first conjunct is false for every row), the scan would run it when it starts
-- shape: trap
-- expect: plain
-- key: -
SELECT w.composition, count(*) AS n
FROM t_wl w
WHERE w.id < 0 AND w.entity_id = ANY (ARRAY(SELECT 100 / (e.id - 5) FROM t_wl_entity e))
GROUP BY 1
ORDER BY 1 NULLS FIRST
