-- Status: pushed; chained lookup keys are verified by agg_lookup_dependencies and coverage WL09.
-- GAP 2: a lookup join keyed by a column of another joined table (t_wl_map.entity_type = t_wl_entity.type) runs as hash joins.
-- Expected: Lookup Joins on both tables. The literal key (sm.entity_type = 'attraction') is pushed.
\echo -- not pushed: key from a joined table
EXPLAIN (COSTS OFF) SELECT sm.mapped_category, count(*) FROM t_wl r JOIN t_wl_entity p ON p.id = r.entity_id
 CROSS JOIN LATERAL jsonb_to_recordset(coldiff.wl_norm(r.doc) -> 'opinions') AS i(category text, subrating text, polarity text, score float8)
 LEFT JOIN t_wl_map sm ON sm.entity_type = p.type AND sm.category = i.category AND sm.subrating = i.subrating
 WHERE r.deleted_at IS NULL GROUP BY 1;
\echo -- pushed: literal key
EXPLAIN (COSTS OFF) SELECT sm.mapped_category, count(*) FROM t_wl r
 CROSS JOIN LATERAL jsonb_to_recordset(coldiff.wl_norm(r.doc) -> 'opinions') AS i(category text, subrating text, polarity text, score float8)
 LEFT JOIN t_wl_map sm ON sm.entity_type = 'attraction' AND sm.category = i.category AND sm.subrating = i.subrating
 WHERE r.deleted_at IS NULL GROUP BY 1;
