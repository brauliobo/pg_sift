-- Status (3.2): fixed: no plan error; the EXISTS over the recordset and the nested jsonpath are pushed
-- GAP 8 (bug + shape). With a sift_col index that holds an EXPRESSION column (coldiff.load_norm(doc)), a correlated EXISTS
-- over the recordset of that expression fails at PLAN time: `ERROR: invalid attnum 18 for relation "o"` (the same query with
-- sift.enable_agg = off, or over the raw column, plans fine). Expected: the normal plan, or the EXISTS as a pushed row filter.
-- A sign or category filter of the workload needs it.
\echo -- plan error
EXPLAIN (COSTS OFF) SELECT count(*) FROM t_load r WHERE r.deleted_at IS NULL AND EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.load_norm(r.doc) -> 'parts') AS o(category text, sign text) WHERE o.sign = 'positive');
\echo -- the jsonpath spelling is not pushed either (a predicate on the elements of a nested array: only a top-level array such as $[*] ? (@.type == "manager") is)
EXPLAIN (COSTS OFF) SELECT count(*) FROM t_load r WHERE r.deleted_at IS NULL AND jsonb_path_exists(coldiff.load_norm(r.doc), '$.parts[*] ? (@.sign == "positive")');
\echo -- fine with the aggregate pushdown off
SET sift.enable_agg = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM t_load r WHERE r.deleted_at IS NULL AND EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.load_norm(r.doc) -> 'parts') AS o(category text, sign text) WHERE o.sign = 'positive');
