-- ALTER EXTENSION pg_sift UPDATE by a superuser, with ordinary roles in the database. At version 3.2 (3.2.0 and 3.2.1) every DROP of a role that
-- is not a superuser fails ("permission denied for schema sift"); the update gives the event trigger functions the extension owner's rights, and
-- the tables, indexes and rows made at 3.2 go on working for the same roles.
-- The released SQL of 3.2 is installed here by CREATE EXTENSION ... VERSION '3.2', on the library of this build.
-- DROP EXTENSION leaves the schema sift, with the USAGE that 3.3 gave PUBLIC, so a database that had 3.3 is cleared of it first.
SET client_min_messages = warning;
DROP SCHEMA IF EXISTS sift;
RESET client_min_messages;
CREATE EXTENSION pg_sift VERSION '3.2';
\set VERBOSITY terse
\set ECHO none
\ir test/roles_helpers.sql
\set ECHO all
SELECT rr_roles_drop('ru_');
CREATE ROLE ru_own;
CREATE ROLE ru_mem IN ROLE ru_own;
CREATE ROLE ru_rd;
CREATE ROLE ru_new;
CREATE SCHEMA ru;
GRANT USAGE ON SCHEMA ru TO ru_rd;
GRANT USAGE, CREATE ON SCHEMA ru TO ru_own, ru_mem;
GRANT USAGE ON SCHEMA sift TO ru_own, ru_mem, ru_rd;
-- 3.2 grants the schema to nobody, and ru_new was not given it
SET enable_seqscan = off;

-- at 3.2: what the roles make works, a drop does not
SELECT * FROM rr_run($$
ru_own | CREATE TABLE ru.docs (id int, body text, g int)
ru_own | INSERT INTO ru.docs SELECT i, 'alpha beta ' || i, i % 3 FROM generate_series(1, 300) i
ru_own | CREATE INDEX docs_idx ON ru.docs USING sift (body)
ru_own | CREATE INDEX docs_col ON ru.docs USING sift_col (id, g)
ru_own | CREATE TABLE ru.facts (id int, g int, doc jsonb) USING sift_table
ru_own | INSERT INTO ru.facts SELECT i, i % 3, '{"a": 1}' FROM generate_series(1, 300) i
ru_own | CREATE TABLE ru.plain (id int)
ru_own | GRANT SELECT ON ru.docs, ru.facts TO ru_rd
ru_own | ALTER TABLE ru.plain ADD COLUMN extra int
ru_own | !SELECT sift.table_set_paths('ru.facts', 'doc', 'doc->a int4')
ru_own | DROP TABLE ru.plain
ru_own | DROP INDEX ru.docs_col
ru_mem | DROP TABLE ru.plain
ru_own | SELECT count(*) FROM ru.docs WHERE body @@> 'alpha'
ru_new | SELECT sift.version()
$$);

SET client_min_messages = error;
ALTER EXTENSION pg_sift UPDATE;
SET client_min_messages = warning;
SELECT extversion FROM pg_extension WHERE extname = 'pg_sift';

-- at 3.6: the same statements work and so does everything that was there
SELECT * FROM rr_run($$
ru_own | DROP TABLE ru.plain
ru_own | DROP INDEX ru.docs_col
ru_own | CREATE TABLE ru.plain (id int)
ru_mem | DROP TABLE ru.plain
ru_own | !SELECT sift.table_set_paths('ru.facts', 'doc', 'doc->a int4')
ru_own | SELECT count(*) FROM ru.docs WHERE body @@> 'alpha'
ru_rd | SELECT count(*) FROM ru.docs WHERE body @@> 'alpha'
ru_rd | SELECT g, count(*) FROM ru.facts GROUP BY g ORDER BY g
ru_own | CREATE INDEX docs_col ON ru.docs USING sift_col (id, g)
ru_own | !SELECT sift.promote('ru.docs_idx')
ru_own | !SELECT sift.col_verify('ru.docs_col', true)
ru_rd | !SELECT * FROM sift.workers
ru_new | SELECT sift.version()
ru_rd | DROP TABLE ru.docs
ru_own | INSERT INTO ru.docs SELECT i + 300, 'alpha gamma ' || i, i % 3 FROM generate_series(1, 20) i
ru_own | SELECT count(*) FROM ru.docs WHERE body @@> 'alpha'
ru_own | DROP TABLE ru.docs
ru_own | DROP TABLE ru.facts
$$);
SELECT count(*) AS paths_left FROM sift.table_paths;

DROP SCHEMA ru CASCADE;
DROP FUNCTION rr_run(text), rr_matrix(text[], text), rr_pushdown_diff(text[], text), rr_script(text[], text), rr_roles_drop(text);
DROP OWNED BY ru_own, ru_mem, ru_rd, ru_new;
DROP ROLE ru_own, ru_mem, ru_rd, ru_new;
DROP EXTENSION pg_sift;
