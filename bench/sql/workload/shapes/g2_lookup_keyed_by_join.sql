-- Status: pushed; chained lookup keys are verified by agg_lookup_dependencies and coverage WL09.
-- GAP 2: a lookup join keyed by a column of another joined table (t_work_map.entity_type = t_work_dim.type) runs as hash joins.
-- Expected: Lookup Joins on both tables. The literal key (sm.entity_type = 'kind_b') is pushed.
\echo -- not pushed: key from a joined table
EXPLAIN (COSTS OFF) SELECT sm.derived_category, count(*) FROM t_work r JOIN t_work_dim p ON p.id = r.dim_a_id
 CROSS JOIN LATERAL jsonb_to_recordset(coldiff.doc_norm(r.doc) -> 'parts') AS i(category text, value_num text, tone text, score float8)
 LEFT JOIN t_work_map sm ON sm.entity_type = p.type AND sm.category = i.category AND sm.value_num = i.value_num
 WHERE r.deleted_at IS NULL GROUP BY 1;
\echo -- pushed: literal key
EXPLAIN (COSTS OFF) SELECT sm.derived_category, count(*) FROM t_work r
 CROSS JOIN LATERAL jsonb_to_recordset(coldiff.doc_norm(r.doc) -> 'parts') AS i(category text, value_num text, tone text, score float8)
 LEFT JOIN t_work_map sm ON sm.entity_type = 'kind_b' AND sm.category = i.category AND sm.value_num = i.value_num
 WHERE r.deleted_at IS NULL GROUP BY 1;
