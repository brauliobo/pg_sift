-- = ANY (ARRAY(SELECT ...)): an uncorrelated initplan array tested as a lookup table over the leaf, with a NULL in the array, an empty one, <> ALL and under OR
-- shape: filter
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.composition,
	   count(*) FILTER (WHERE w.entity_id = ANY (ARRAY(SELECT e.id FROM t_wl_entity e WHERE e.type = 'accommodation'))) AS in_type,
	   count(*) FILTER (WHERE w.entity_id <> ALL (ARRAY(SELECT e.id FROM t_wl_entity e WHERE e.type = 'accommodation'))) AS not_in_type,
	   count(*) FILTER (WHERE w.entity_id = ANY (ARRAY(SELECT e.id FROM t_wl_entity e WHERE e.id < 0))) AS empty_array,
	   count(*) FILTER (WHERE w.product_id = ANY (ARRAY(SELECT x.product_id FROM t_wl x WHERE x.id <= 40))) AS with_null,
	   count(*) FILTER (WHERE NOT (w.product_id = ANY (ARRAY(SELECT x.product_id FROM t_wl x WHERE x.id <= 40)))) AS not_with_null,
	   count(*) FILTER (WHERE w.source_id = ANY (ARRAY(SELECT s.id FROM t_wl_source s WHERE s.kind = 'web')) OR w.entity_id = ANY (ARRAY(SELECT e.id FROM t_wl_entity e WHERE e.id <= 3))) AS either
FROM t_wl w
WHERE w.deleted_at IS NULL
GROUP BY 1
ORDER BY 1 NULLS FIRST
