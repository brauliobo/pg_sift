-- = ANY (ARRAY(SELECT ...)): an uncorrelated initplan array tested as a lookup table over the leaf, with a NULL in the array, an empty one, <> ALL and under OR
-- shape: filter
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.label_a,
	   count(*) FILTER (WHERE w.dim_a_id = ANY (ARRAY(SELECT e.id FROM t_work_dim e WHERE e.type = 'kind_a'))) AS in_type,
	   count(*) FILTER (WHERE w.dim_a_id <> ALL (ARRAY(SELECT e.id FROM t_work_dim e WHERE e.type = 'kind_a'))) AS not_in_type,
	   count(*) FILTER (WHERE w.dim_a_id = ANY (ARRAY(SELECT e.id FROM t_work_dim e WHERE e.id < 0))) AS empty_array,
	   count(*) FILTER (WHERE w.foreign_id_b = ANY (ARRAY(SELECT x.foreign_id_b FROM t_work x WHERE x.id <= 40))) AS with_null,
	   count(*) FILTER (WHERE NOT (w.foreign_id_b = ANY (ARRAY(SELECT x.foreign_id_b FROM t_work x WHERE x.id <= 40)))) AS not_with_null,
	   count(*) FILTER (WHERE w.lookup_id = ANY (ARRAY(SELECT s.id FROM t_work_src s WHERE s.kind = 'web')) OR w.dim_a_id = ANY (ARRAY(SELECT e.id FROM t_work_dim e WHERE e.id <= 3))) AS either
FROM t_work w
WHERE w.deleted_at IS NULL
GROUP BY 1
ORDER BY 1 NULLS FIRST
