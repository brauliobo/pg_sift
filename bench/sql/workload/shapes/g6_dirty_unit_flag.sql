-- GAP 6: one string-valued number in a float8 path (or one non-object document in a column with paths) flags its whole unit;
-- with the rows spread over every unit (3 % string scores) no scan that reads the path is pushed down any more, silently.
-- Expected: the unit stays usable (recheck the flagged rows from the heap), as pg_sift 3.0.1 announced.
CREATE SCHEMA IF NOT EXISTS wl_scratch;
DROP TABLE IF EXISTS wl_scratch.dirty;
CREATE TABLE wl_scratch.dirty AS SELECT id, deleted_at, doc FROM t_work WHERE jsonb_typeof(doc) = 'object';
CREATE INDEX dirty_ix ON wl_scratch.dirty USING sift_col (id, deleted_at, doc) WITH (paths = 'doc.parts[]{category text, score float8}');
VACUUM ANALYZE wl_scratch.dirty;
SELECT count(*) FILTER (WHERE jsonb_path_exists(doc, '$.parts[*].score ? (@.type() == "string")')) AS string_scores, count(*) FROM wl_scratch.dirty;
\echo -- not pushed (units flagged)
EXPLAIN (COSTS OFF) SELECT i.category, avg(i.score) FROM wl_scratch.dirty r, jsonb_to_recordset(r.doc -> 'parts') AS i(category text, score float8) WHERE r.deleted_at IS NULL GROUP BY 1;
\echo -- pushed once the strings are numbers (check in a NEW session: the session that repaired the data keeps planning the old flagged units)
UPDATE wl_scratch.dirty SET doc = coldiff.doc_norm(doc);
VACUUM FULL wl_scratch.dirty; -- the unit flags only clear once the heap is rewritten (the UPDATE and a plain VACUUM + REINDEX keep them)
REINDEX INDEX wl_scratch.dirty_ix;
VACUUM ANALYZE wl_scratch.dirty;
EXPLAIN (COSTS OFF) SELECT i.category, avg(i.score) FROM wl_scratch.dirty r, jsonb_to_recordset(r.doc -> 'parts') AS i(category text, score float8) WHERE r.deleted_at IS NULL GROUP BY 1;
