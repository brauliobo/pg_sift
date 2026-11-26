-- a jsonpath filter on the elements of a jsonb array (`jsonb_path_exists(doc, '$.items[*] ? (@.kind == "k_1" && @.score >= 8)')`), on pg_sift its same-element `EXISTS` over `jsonb_to_recordset`: both tests on one element, read from the leaves of the array
-- label: one array element by kind and score (`jsonb_path_exists` on PostgreSQL, `EXISTS` over `jsonb_to_recordset` on pg_sift), `count(*)` and `sum(amount)` by status of sf_events; the variant `gin` is a heap copy with `GIN (doc jsonb_path_ops)`
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS c_jsonb_path_exists_gin__gin CASCADE;
CREATE SCHEMA c_jsonb_path_exists_gin__gin;
CREATE TABLE c_jsonb_path_exists_gin__gin.sf_events WITH (autovacuum_enabled = false) AS SELECT * FROM sf_events;
CREATE INDEX sf_events_doc_gin ON c_jsonb_path_exists_gin__gin.sf_events USING gin (doc jsonb_path_ops);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_jsonb_path_exists_gin__gin.sf_events
--@ sift
SELECT status, count(*) AS n, sum(amount) AS amount FROM sf_events s
WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(s.doc -> 'items') i(kind text, score float8) WHERE i.kind = 'k_1' AND i.score >= 8) GROUP BY 1 ORDER BY 1
--@ pg
SELECT status, count(*) AS n, sum(amount) AS amount FROM sf_events WHERE jsonb_path_exists(doc, '$.items[*] ? (@.kind == "k_1" && @.score >= 8)') GROUP BY 1 ORDER BY 1
