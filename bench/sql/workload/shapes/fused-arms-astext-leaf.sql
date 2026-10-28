-- Status (3.2): fixed: the arms fuse over a text leaf with the cast in the aggregate
-- fused-arms-astext-leaf: the fused per-item arms over a text leaf with the cast in the aggregate (the scores of the documents are text: numbers, numbers as
-- strings and text, so the cast is guarded). Was pushed arm by arm only, one row per item back to the executor.
-- Expected: one SiftAgg, `Pushed Aggregates: avg(inner), count(DISTINCT row)`.
EXPLAIN (COSTS OFF) WITH b AS (SELECT r.id rid, i.category k, CASE WHEN i.score ~ '^-?[0-9]+(\.[0-9]+)?$' THEN i.score::float8 END n
  FROM t_work r, jsonb_to_recordset(coldiff.wl_txt(r.doc) -> 'parts') AS i(category text, score text) WHERE r.deleted_at IS NULL)
SELECT k, avg(a), count(DISTINCT rid) FROM (SELECT rid, k, avg(n) a FROM b GROUP BY rid, k UNION ALL SELECT rid, NULL::text, avg(n) FROM b GROUP BY rid) x GROUP BY k;
