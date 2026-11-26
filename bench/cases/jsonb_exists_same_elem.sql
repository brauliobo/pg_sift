-- an `EXISTS` over the elements of a jsonb array with two tests on one element (`p.dir_a = 'pos' AND p.score >= 8`): a semi-join on the leaves of the array, no element unnested above it
-- label: `EXISTS (SELECT 1 FROM jsonb_to_recordset(doc -> 'parts') ...)` on one element, `count(*)` and `sum(amount)` by kind of the live events; the variant `gin` is a heap copy with `GIN (doc jsonb_path_ops)`
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS c_jsonb_exists_same_elem__gin CASCADE;
CREATE SCHEMA c_jsonb_exists_same_elem__gin;
CREATE TABLE c_jsonb_exists_same_elem__gin.events WITH (autovacuum_enabled = false) AS SELECT * FROM events;
CREATE INDEX events_doc_gin ON c_jsonb_exists_same_elem__gin.events USING gin (doc jsonb_path_ops);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_jsonb_exists_same_elem__gin.events
--@ pg
SELECT e.kind, count(*) AS n, sum(e.amount) AS amount FROM events e WHERE e.deleted_at IS NULL
AND EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc -> 'parts') p(kind text, dir_a text, score float8) WHERE p.dir_a = 'pos' AND p.score >= 8) GROUP BY 1 ORDER BY 1
