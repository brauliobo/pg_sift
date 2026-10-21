-- Status (3.2): partial: the arms are pushed one by one; the fused scan counts distinct rows only (docs/AGG.md, "Workload shapes")
-- GAP 4: per-item arms under an outer aggregate (fused scan, `Pushed Aggregates: avg(inner), count(DISTINCT row)`) lose the fusion
-- as soon as the outer query holds a second count(DISTINCT) over another column (entities_count next to the items count).
-- Expected: one SiftAgg for both.
\echo -- fused: one count(DISTINCT)
EXPLAIN (COSTS OFF) WITH b AS (SELECT r.id rid, r.entity_id pid, i.category k, i.score n FROM t_load r, jsonb_to_recordset(coldiff.load_norm(r.doc) -> 'parts') AS i(category text, score float8) WHERE r.deleted_at IS NULL)
SELECT k, avg(a), count(DISTINCT rid) FROM (SELECT rid, k, avg(n) a FROM b GROUP BY rid, k UNION ALL SELECT rid, NULL::text, avg(n) FROM b GROUP BY rid) x GROUP BY k;
\echo -- not fused: second count(DISTINCT pid)
EXPLAIN (COSTS OFF) WITH b AS (SELECT r.id rid, r.entity_id pid, i.category k, i.score n FROM t_load r, jsonb_to_recordset(coldiff.load_norm(r.doc) -> 'parts') AS i(category text, score float8) WHERE r.deleted_at IS NULL)
SELECT k, avg(a), count(DISTINCT rid), count(DISTINCT pid) FROM (SELECT rid, pid, k, avg(n) a FROM b GROUP BY rid, pid, k UNION ALL SELECT rid, pid, NULL::text, avg(n) FROM b GROUP BY rid, pid) x GROUP BY k;
