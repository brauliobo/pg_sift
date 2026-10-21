-- Status: pushed; chained lookup keys are verified by agg_lookup_dependencies and coverage WL09.
-- A lookup key can read a column of another joined table; both joined-key and literal-key variants are pushed.
-- Expected: Lookup Joins on both tables for the joined-key variant.
\echo -- pushed: key from a joined table
EXPLAIN (COSTS OFF) SELECT sm.mapped_category, count(*) FROM t_load r JOIN t_load_entity p ON p.id = r.entity_id
 CROSS JOIN LATERAL jsonb_to_recordset(coldiff.load_norm(r.doc) -> 'parts') AS i(category text, val text, sign text, score float8)
 LEFT JOIN t_load_map sm ON sm.entity_type = p.type AND sm.category = i.category AND sm.val = i.val
 WHERE r.deleted_at IS NULL GROUP BY 1;
\echo -- pushed: literal key
EXPLAIN (COSTS OFF) SELECT sm.mapped_category, count(*) FROM t_load r
 CROSS JOIN LATERAL jsonb_to_recordset(coldiff.load_norm(r.doc) -> 'parts') AS i(category text, val text, sign text, score float8)
 LEFT JOIN t_load_map sm ON sm.entity_type = 'cat_b' AND sm.category = i.category AND sm.val = i.val
 WHERE r.deleted_at IS NULL GROUP BY 1;
