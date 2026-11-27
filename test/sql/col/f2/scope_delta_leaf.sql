-- A sift_col index names a leaf in its column deltas by the leaf plus one, not by its attribute: with a jsonb column of two leaves first, the leaf of a text column the scope summary does
-- not read is the number of the attribute of the member. An UPDATE of that text column takes a column delta, and the summary still serves the scope read and equals the heap (S120-18). Every check prints t.
-- requires: build col
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_sift;
-- the scope summary is Format 2.0; the testing library builds new indexes in Format 1.0 when told to
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

CREATE TABLE parents (entity_id int NOT NULL, parent_id int NOT NULL, PRIMARY KEY (entity_id, parent_id)) WITH (autovacuum_enabled = false);
INSERT INTO parents SELECT e, 1 + e % 7 FROM generate_series(1, 200) e;
CREATE TABLE ev (id int PRIMARY KEY, doc jsonb, note text, entity_id int NOT NULL, day date NOT NULL, qty int) WITH (autovacuum_enabled = false);
CREATE TABLE evh (LIKE ev INCLUDING ALL) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT g, jsonb_build_object('flag', CASE WHEN g % 3 = 0 THEN 'a' ELSE 'b' END), 'note ' || g, 1 + g % 200, date '2030-01-01' + g % 90, g % 13 FROM generate_series(1, 3000) g;
INSERT INTO evh SELECT * FROM ev;
CREATE INDEX ev_sift ON ev USING sift_col (doc, note, entity_id, day, qty)
	WITH (key_order = 'entity_id, day', paths = 'doc.flag text', scope = 'parent_id: entity_id -> parents(entity_id, parent_id)');

CREATE FUNCTION scope_read() RETURNS text LANGUAGE plpgsql AS $$
DECLARE p jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off, BUFFERS off, FORMAT JSON) SELECT p.parent_id, count(*), sum(e.qty) FROM ev e JOIN parents p USING (entity_id) GROUP BY p.parent_id' INTO p;
	RETURN jsonb_path_query_first(p, '$.**."Scope Read"') #>> '{}';
END $$;
CREATE FUNCTION same_rows(a text, b text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM ((%s EXCEPT ALL %s) UNION ALL (%s EXCEPT ALL %s)) d', a, b, b, a) INTO n;
	RETURN n = 0;
END $$;
CREATE FUNCTION scoped() RETURNS boolean LANGUAGE sql AS $$
	SELECT same_rows('SELECT p.parent_id, count(*), sum(e.qty), max(e.day) FROM ev e JOIN parents p USING (entity_id) GROUP BY p.parent_id',
					 'SELECT p.parent_id, count(*), sum(e.qty), max(e.day) FROM evh e JOIN parents p USING (entity_id) GROUP BY p.parent_id') $$;

SELECT array_agg(leaf ORDER BY ord) = '{doc.flag,doc#present,note,entity_id}' AS jsonb_leaves_come_first
FROM (SELECT leaf, min(ord) AS ord FROM sift.col_sections('ev_sift') WITH ORDINALITY s(section, leaf, bytes, ord) WHERE section = 'chunks' GROUP BY leaf) l WHERE ord <= 4;
SELECT scope_read() = 'summary' AS summary_serves_when_built;

UPDATE ev SET note = note || ' edited' WHERE id % 10 = 0;
UPDATE evh SET note = note || ' edited' WHERE id % 10 = 0;
SELECT coalesce(sum(dead_rows), 0) = 0 AS note_update_is_a_column_delta FROM sift.parts('ev_sift');
SELECT scope_read() = 'summary' AS summary_serves_after_note_update;
SELECT scoped() AS scoped_equals_heap;
SELECT same_rows('SELECT id, note FROM ev WHERE id % 10 = 0', 'SELECT id, note FROM evh WHERE id % 10 = 0') AS patched_rows_equal_heap;
DROP TABLE ev, evh, parents;
DROP FUNCTION scoped(), scope_read(), same_rows(text, text);
DROP EXTENSION pg_sift;
