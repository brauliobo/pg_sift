-- The paths of the jsonb columns of an index with no paths option (or paths = 'auto') are read once, at CREATE INDEX, from a sample of their documents
-- (sift.suggest_paths), written to the option as 'auto: spec' so that a rebuild keeps them, and said in a NOTICE. The answers are the plain plan's, and an index with paths written by hand is untouched.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = notice;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE pa (id int, doc jsonb, tail jsonb) WITH (autovacuum_enabled = off);
INSERT INTO pa SELECT i, jsonb_build_object('name', 'n' || i, 'count', i % 100, 'meta', jsonb_build_object('lang', 'l' || i % 5),
	'parts', jsonb_build_array(jsonb_build_object('cat', 'c' || i % 4, 'score', i % 10))), jsonb_build_object('t', i % 7) FROM generate_series(1, 3000) i;
VACUUM (FREEZE, ANALYZE) pa;

CREATE FUNCTION pa_option(i text) RETURNS text LANGUAGE sql AS
$$ SELECT coalesce((SELECT o FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = i AND o LIKE 'paths=%'), 'not set') $$;
CREATE FUNCTION pa_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config(s, (NOT push)::text, true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), ''no rows'') FROM (%s) x', q) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION pa_pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config(s, 'false', true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%SiftAgg%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
CREATE TABLE pa_q (n int, q text);
INSERT INTO pa_q VALUES
	(1, 'SELECT doc->''meta''->>''lang'', count(*) FROM pa GROUP BY 1'),
	(2, 'SELECT (doc->>''count'')::int, count(*) FROM pa WHERE doc->>''name'' > ''n5'' GROUP BY 1'),
	(3, 'SELECT e->>''cat'', count(*), sum((e->>''score'')::float8) FROM pa, jsonb_array_elements(doc->''parts'') e GROUP BY 1'),
	(4, 'SELECT tail->>''t'', count(*) FROM pa GROUP BY 1');

-- no paths option: decided at CREATE INDEX (a jsonb column needed paths, so this was an error): the NOTICE, the option, and the answers
CREATE INDEX pa_auto ON pa USING sift_col (id, doc, tail);
SELECT pa_option('pa_auto');
SELECT sift.col_verify('pa_auto', true);
SELECT n, pa_rows(q, true) = pa_rows(q, false) AS same, pa_pushed(q) AS pushed FROM pa_q ORDER BY n;

-- a rebuild keeps the paths, whatever the table holds now
INSERT INTO pa SELECT i, jsonb_build_object('other', i), NULL FROM generate_series(3001, 6000) i;
REINDEX INDEX pa_auto;
SELECT pa_option('pa_auto');
-- the index still verifies after the table changed, and answers as the plain plan does
SELECT sift.col_verify('pa_auto', true);
SELECT n, pa_rows(q, true) = pa_rows(q, false) AS same FROM pa_q ORDER BY n;
DROP INDEX pa_auto;
DELETE FROM pa WHERE id > 3000;

-- paths by hand are not touched; 'auto' by name is the same as none
CREATE INDEX pa_hand ON pa USING sift_col (id, doc) WITH (paths = 'doc.name text');
SELECT pa_option('pa_hand');
CREATE INDEX pa_none ON pa USING sift_col (id, doc) WITH (paths = 'auto');
SELECT pa_option('pa_none');
-- a map of keys that are all different, of one type: a wildcard
CREATE TABLE pa_few (id int, doc jsonb) WITH (autovacuum_enabled = off);
INSERT INTO pa_few SELECT i, jsonb_build_object('k' || i, i) FROM generate_series(1, 2000) i;
CREATE INDEX pa_few_col ON pa_few USING sift_col (id, doc);
SELECT pa_option('pa_few_col');
-- a table without rows has no key that qualifies: the refusal of a jsonb column without paths stands
CREATE TABLE pa_empty (id int, doc jsonb);
CREATE INDEX pa_empty_col ON pa_empty USING sift_col (id, doc);
SELECT pa_option('pa_empty_col');

-- ALTER INDEX ... SET (paths = 'auto') or RESET (paths) asks for the documents to be read again at the next REINDEX (as after any change of paths the
-- index has no layout until then, so the REINDEX follows at once)
CREATE TABLE pa_re (id int, doc jsonb) WITH (autovacuum_enabled = off);
INSERT INTO pa_re SELECT i, jsonb_build_object('a', i % 7) FROM generate_series(1, 2000) i;
CREATE INDEX pa_re_col ON pa_re USING sift_col (id, doc);
SELECT pa_option('pa_re_col');
DELETE FROM pa_re;
INSERT INTO pa_re SELECT i, jsonb_build_object('b', i % 11, 'label', 'l' || i % 3) FROM generate_series(1, 2000) i;
ALTER INDEX pa_re_col SET (paths = 'auto');
SELECT pa_option('pa_re_col');
REINDEX INDEX pa_re_col;
SELECT pa_option('pa_re_col');
DELETE FROM pa_re;
INSERT INTO pa_re SELECT i, jsonb_build_object('c', i % 5) FROM generate_series(1, 2000) i;
ALTER INDEX pa_re_col RESET (paths);
REINDEX INDEX pa_re_col;
SELECT pa_option('pa_re_col');
SELECT sift.col_verify('pa_re_col', true);

DROP TABLE pa, pa_q, pa_few, pa_empty, pa_re;
DROP FUNCTION pa_option(text), pa_rows(text, bool), pa_pushed(text);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
