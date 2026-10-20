-- GAP 7: the fused per-item arms are not offered for a selective scan (few t_wl_entity and a short period), even with every other path
-- priced out; the flat arms are (ratings). A cost decision at this size, to recheck at 3 M rows.
\echo -- one entity + a period: not pushed
EXPLAIN (COSTS OFF) WITH b AS (SELECT r.id rid, i.category k, i.score n FROM t_wl r, jsonb_to_recordset(coldiff.wl_norm(r.doc) -> 'opinions') AS i(category text, score float8) WHERE r.deleted_at IS NULL AND r.entity_id = 900 AND r.item_date >= '2025-01-01')
SELECT k, avg(a), count(DISTINCT rid) FROM (SELECT rid, k, avg(n) a FROM b GROUP BY rid, k UNION ALL SELECT rid, NULL::text, avg(n) FROM b GROUP BY rid) x GROUP BY k;
\echo -- one entity only: pushed
EXPLAIN (COSTS OFF) WITH b AS (SELECT r.id rid, i.category k, i.score n FROM t_wl r, jsonb_to_recordset(coldiff.wl_norm(r.doc) -> 'opinions') AS i(category text, score float8) WHERE r.deleted_at IS NULL AND r.entity_id = 900)
SELECT k, avg(a), count(DISTINCT rid) FROM (SELECT rid, k, avg(n) a FROM b GROUP BY rid, k UNION ALL SELECT rid, NULL::text, avg(n) FROM b GROUP BY rid) x GROUP BY k;
