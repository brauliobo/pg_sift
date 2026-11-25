-- DDL by a role that is neither a superuser nor the extension's owner and has no privilege on the sift schema: the event triggers of the
-- extension fire for it on every DROP and ALTER TABLE (3.2.0 and 3.2.1: every DROP failed with "permission denied for schema sift")
CREATE EXTENSION pg_sift;
\set VERBOSITY terse
SET client_min_messages = warning;
DROP ROLE IF EXISTS sift_nsd_owner, sift_nsd_other;
CREATE ROLE sift_nsd_owner;
CREATE ROLE sift_nsd_other;
CREATE SCHEMA nsd AUTHORIZATION sift_nsd_owner;
CREATE SCHEMA nsd_cascade AUTHORIZATION sift_nsd_owner;
GRANT USAGE ON SCHEMA nsd TO sift_nsd_other;

-- runs each statement as the current role and reports the error it raised, if any
CREATE FUNCTION nsd_run(stmts text[]) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE s text;
BEGIN
	FOREACH s IN ARRAY stmts LOOP
		BEGIN
			EXECUTE s;
			RETURN NEXT 'ok: ' || s;
		EXCEPTION WHEN OTHERS THEN
			RETURN NEXT s || ' -> ' || SQLERRM;
		END;
	END LOOP;
END $$;

SET ROLE sift_nsd_owner;
SELECT * FROM nsd_run(ARRAY[
	-- a table without a sift index
	'CREATE TABLE nsd.plain (id int, body text)',
	'DROP TABLE nsd.plain',
	-- sift index: DROP INDEX and DROP TABLE
	'CREATE TABLE nsd.docs (id int, body text)',
	'INSERT INTO nsd.docs SELECT i, ''alpha '' || i FROM generate_series(1, 300) i',
	'CREATE INDEX docs_idx ON nsd.docs USING sift (body)',
	'DROP INDEX nsd.docs_idx',
	'CREATE INDEX docs_col ON nsd.docs USING sift_col (id)',
	'DROP INDEX nsd.docs_col',
	'CREATE INDEX docs_idx ON nsd.docs USING sift (body)',
	'TRUNCATE nsd.docs',
	'ALTER TABLE nsd.docs RENAME TO docs2',
	'ALTER TABLE nsd.docs2 RENAME COLUMN body TO txt',
	'ALTER TABLE nsd.docs2 ADD COLUMN extra int',
	'ALTER TABLE nsd.docs2 ALTER COLUMN extra TYPE bigint',
	'DROP TABLE nsd.docs2',
	-- sift_table
	'CREATE TABLE nsd.facts (id int, ts timestamptz, val int, doc jsonb) USING sift_table',
	'ALTER TABLE nsd.facts ADD COLUMN more int',
	'ALTER TABLE nsd.facts RENAME COLUMN more TO more2',
	'DROP TABLE nsd.facts',
	-- a schema with both kinds of table
	'CREATE TABLE nsd_cascade.a (id int, body text)',
	'CREATE INDEX a_idx ON nsd_cascade.a USING sift (body)',
	'CREATE TABLE nsd_cascade.b (id int, val int) USING sift_table',
	'DROP SCHEMA nsd_cascade CASCADE'
]);
RESET ROLE;

-- the declared paths of a table: set by its owner (who needs USAGE on sift to call the function), refused to another role, and gone
-- with the table
GRANT USAGE ON SCHEMA sift TO sift_nsd_owner, sift_nsd_other;
SET ROLE sift_nsd_owner;
CREATE TABLE nsd.facts (id int, doc jsonb) USING sift_table;
SELECT * FROM nsd_run(ARRAY[
	'SELECT sift.table_set_paths(''nsd.facts'', ''doc'', ''doc->a int4'')'
]);
RESET ROLE;
SELECT attname, spec FROM sift.table_paths WHERE relid = 'nsd.facts'::regclass;
SET ROLE sift_nsd_other;
SELECT * FROM nsd_run(ARRAY[
	'SELECT sift.table_set_paths(''nsd.facts'', ''doc'', ''doc->b int4'')',
	'SELECT sift.table_set_paths(''nsd.facts'', ''doc'', NULL)'
]);
RESET ROLE;
SELECT attname, spec FROM sift.table_paths WHERE relid = 'nsd.facts'::regclass;
SET ROLE sift_nsd_owner;
SELECT * FROM nsd_run(ARRAY[
	'SELECT sift.table_set_paths(''nsd.facts'', ''doc'', NULL)',
	'SELECT sift.table_set_paths(''nsd.facts'', ''doc'', ''doc->a int4'')',
	'DROP TABLE nsd.facts'
]);
RESET ROLE;
SELECT count(*) AS paths_left FROM sift.table_paths;

DROP FUNCTION nsd_run(text[]);
DROP OWNED BY sift_nsd_owner, sift_nsd_other;
DROP ROLE sift_nsd_owner, sift_nsd_other;
DROP EXTENSION pg_sift;
