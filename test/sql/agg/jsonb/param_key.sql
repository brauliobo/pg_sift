-- A jsonb key test (doc ? 'parts') next to a qual that reads a parameter: the runtime key of the parameter pointed into the array of the scan keys before
-- the jsonb keys were appended to a new one, so the scan read 0 for the parameter. A generic plan counted 0 rows where the normal plan counts 215, and a
-- correlated subquery on a text key read a NULL text and crashed.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET seq_page_cost = 1000; SET random_page_cost = 1000; SET max_parallel_workers_per_gather = 0;
CREATE TABLE ev (id int PRIMARY KEY, grp int, kind text, doc jsonb);
INSERT INTO ev SELECT i, i % 12, 'k' || i % 5, CASE WHEN i % 12 < 3 THEN NULL WHEN i % 7 = 0 THEN '{}'::jsonb ELSE '{"parts": [1]}' END FROM generate_series(1, 3000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, grp, kind, doc);
VACUUM ANALYZE ev;
SET plan_cache_mode = force_generic_plan;
PREPARE by_grp(int) AS SELECT count(*) FROM ev WHERE grp = $1 AND doc ? 'parts';
SET sift.enable_agg = off;
CREATE TABLE ref_grp AS EXECUTE by_grp(5);
CREATE TABLE ref_kind AS SELECT e.id, (SELECT f.id FROM ev f WHERE f.kind = e.kind AND f.doc ? 'parts' ORDER BY f.grp DESC, f.id LIMIT 1) AS first_id FROM ev e WHERE e.id % 500 < 3;
SET sift.enable_agg = on;
CREATE TABLE got_grp AS EXECUTE by_grp(5);
CREATE TABLE got_kind AS SELECT e.id, (SELECT f.id FROM ev f WHERE f.kind = e.kind AND f.doc ? 'parts' ORDER BY f.grp DESC, f.id LIMIT 1) AS first_id FROM ev e WHERE e.id % 500 < 3;
SELECT g.count, g.count = r.count AS same FROM got_grp g, ref_grp r;
SELECT g.id, g.first_id, g.first_id IS NOT DISTINCT FROM r.first_id AS same FROM got_kind g JOIN ref_kind r USING (id) ORDER BY g.id;
DROP TABLE got_grp, ref_grp, got_kind, ref_kind, ev;
DROP EXTENSION pg_sift;
