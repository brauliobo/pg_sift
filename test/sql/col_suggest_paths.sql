-- sift.suggest_paths(rel, column, sample): the paths option that a jsonb column asks for, read from a sample of its documents (evenly spaced blocks: the same
-- answer for the same table). Typed paths for scalar keys (int4, int8 beyond 32 bits, float8 with fractions, text, bool, astext for a mix), a wildcard for a map of
-- more than 4 keys of one type, a group for an array of objects; a rare key, one without a single shape and free text (values over 64 bytes on average) are left out. The answer is a spec the option accepts,
-- an index built with it verifies, and what it pushes answers as the plain plan does.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE sp (id int, doc jsonb, other jsonb) WITH (autovacuum_enabled = off);
INSERT INTO sp SELECT i, jsonb_build_object(
	'name', 'n' || i, 'count', i % 100, 'big', i::bigint * 10000000, 'ratio', i / 7.0, 'ok', i % 2 = 0,
	'mixed', CASE WHEN i % 3 = 0 THEN to_jsonb(i) ELSE to_jsonb('s' || i) END,
	'rare', CASE WHEN i % 500 = 0 THEN to_jsonb(1) ELSE 'null'::jsonb END,
	'meta', jsonb_build_object('lang', 'l' || i % 5, 'depth', i % 9),
	'tags', jsonb_build_array('a' || i % 3, 'b'),
	'parts', jsonb_build_array(jsonb_build_object('cat', 'c' || i % 4, 'score', i % 10), jsonb_build_object('cat', 'd', 'score', 2.5)),
	'weird key', 1,
	'note', repeat('free text ', 10) || i,
	'scores', jsonb_build_object('a', i % 5 + 0.5, 'b', i % 6 + 0.5, 'c', 1.5, 'd', 2.5, 'e', i % 7 + 0.5),
	'metrics', (SELECT jsonb_object_agg('m' || g, g % 7) FROM generate_series(1, 40) g),
	'shape', CASE WHEN i % 2 = 0 THEN '[1]'::jsonb ELSE '{"a":1}'::jsonb END), NULL FROM generate_series(1, 3000) i;
VACUUM (FREEZE, ANALYZE) sp;

SELECT sift.suggest_paths('sp', 'doc');
-- the same answer again, and with a smaller sample the keys are those of the documents it reads
SELECT sift.suggest_paths('sp', 'doc') = sift.suggest_paths('sp', 'doc', 2000) AS same_default, sift.suggest_paths('sp', 'doc', 50) IS NOT NULL AS small_sample;
-- no document, no key
SELECT sift.suggest_paths('sp', 'other');

-- the index of the suggestion builds, verifies, and answers like the plain plan
SELECT format('CREATE INDEX sp_col ON sp USING sift_col (id, doc) WITH (paths = %L)', sift.suggest_paths('sp', 'doc')) AS ddl \gset
:ddl;
SELECT sift.col_verify('sp_col', true);
CREATE FUNCTION sp_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config(s, (NOT push)::text, true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), ''no rows'') FROM (%s) x', q) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION sp_pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config(s, 'false', true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%SiftAgg%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
SELECT n, sp_rows(q, true) = sp_rows(q, false) AS same, sp_pushed(q) AS pushed FROM (VALUES
	(1, 'SELECT doc->''meta''->>''lang'', count(*) FROM sp GROUP BY 1'),
	(2, 'SELECT (doc->>''count'')::int, count(*) FROM sp WHERE doc->>''name'' > ''n5'' GROUP BY 1'),
	(3, 'SELECT e->>''cat'', count(*), sum((e->>''score'')::float8) FROM sp, jsonb_array_elements(doc->''parts'') e GROUP BY 1'),
	(4, 'SELECT doc->>''mixed'', count(*) FROM sp WHERE doc->>''mixed'' LIKE ''s1%'' GROUP BY 1'),
	(5, 'SELECT sum((doc->>''big'')::bigint), count(*) FROM sp WHERE (doc->''ok'')::bool')) AS t(n, q) ORDER BY n;

-- documents that are arrays: a group of the column's own path, without the arrays inside its elements (sift_col holds one level of them); a list of scalars
CREATE TABLE sp_arr (id int, doc jsonb, nums jsonb) WITH (autovacuum_enabled = off);
INSERT INTO sp_arr SELECT i, CASE WHEN i % 3 = 0 THEN NULL ELSE jsonb_build_array(jsonb_build_object('type', 't' || i % 4, 'comment', 'c' || i, 'tags', jsonb_build_array('a', 'b'))) END,
	jsonb_build_array(i % 5, i % 7) FROM generate_series(1, 3000) i;
SELECT sift.suggest_paths('sp_arr', 'doc');
SELECT sift.suggest_paths('sp_arr', 'nums');
CREATE INDEX sp_arr_col ON sp_arr USING sift_col (id, doc, nums);
SELECT sp_rows('SELECT e->>''type'', count(*) FROM sp_arr, jsonb_array_elements(doc) e GROUP BY 1', true) = sp_rows('SELECT e->>''type'', count(*) FROM sp_arr, jsonb_array_elements(doc) e GROUP BY 1', false) AS same_group;
SELECT sift.col_verify('sp_arr_col', true);

-- errors
SELECT sift.suggest_paths('sp', 'id');
SELECT sift.suggest_paths('sp', 'nope');
SELECT sift.suggest_paths('sp_col', 'doc');
SELECT sift.suggest_paths('sp', 'doc', 0);
CREATE ROLE sp_user NOLOGIN;
SET ROLE sp_user;
SELECT sift.suggest_paths('sp', 'doc');
RESET ROLE;
GRANT SELECT ON sp TO sp_user;
ALTER TABLE sp ENABLE ROW LEVEL SECURITY;
CREATE POLICY sp_all ON sp FOR SELECT USING (id < 10);
SET ROLE sp_user;
SELECT sift.suggest_paths('sp', 'doc');
RESET ROLE;

DROP TABLE sp, sp_arr;
DROP FUNCTION sp_rows(text, bool), sp_pushed(text);
DROP ROLE sp_user;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
