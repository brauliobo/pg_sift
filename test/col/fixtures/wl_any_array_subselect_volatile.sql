-- trap: a volatile function in the ARRAY sub-select
-- shape: trap
-- expect: plain
-- key: -
SELECT w.composition, count(*) AS n
FROM t_wl w
WHERE w.entity_id = ANY (ARRAY(SELECT e.id FROM t_wl_entity e WHERE e.id <= 5 AND random() < 2))
GROUP BY 1
ORDER BY 1 NULLS FIRST
