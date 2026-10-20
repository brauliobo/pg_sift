-- GAP 5: the fused per-item arms need a numeric typed leaf. The same query over an `astext` leaf with the cast in the
-- aggregate (avg(n::float8)) is pushed arm by arm only: one row per item goes back to the executor, nothing is fused
-- (33x slower than the fused form at 300k items, same as the plain plan). A typed leaf flags a whole unit for one string-valued
-- number, so a client normalizes through coldiff.wl_norm instead of reading astext.
-- Expected: the astext + cast form fuses too, or a typed leaf takes a string-valued number by rechecking the row.
CREATE SCHEMA IF NOT EXISTS wl_scratch;
DROP TABLE IF EXISTS wl_scratch.rv_text;
CREATE TABLE wl_scratch.rv_text AS SELECT id, deleted_at, doc FROM t_wl;
ALTER TABLE wl_scratch.rv_text ADD PRIMARY KEY (id);
CREATE INDEX rv_text_ix ON wl_scratch.rv_text USING sift_col (id, deleted_at, (CASE WHEN jsonb_typeof(doc) = 'object' THEN doc END))
  WITH (paths = '"case".opinions[]{category astext, score astext}');
VACUUM ANALYZE wl_scratch.rv_text;
EXPLAIN (COSTS OFF) WITH b AS (SELECT r.id rid, i.category k, i.score n FROM wl_scratch.rv_text r, jsonb_to_recordset((CASE WHEN jsonb_typeof(r.doc) = 'object' THEN r.doc END) -> 'opinions') AS i(category text, score text) WHERE r.deleted_at IS NULL)
SELECT k, avg(a), count(DISTINCT rid) FROM (SELECT rid, k, avg(n::float8) a FROM b GROUP BY rid, k UNION ALL SELECT rid, NULL::text, avg(n::float8) FROM b GROUP BY rid) x GROUP BY k;
