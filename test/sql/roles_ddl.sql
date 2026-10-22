-- DDL as ordinary roles: the owner of a table (a role that has only CREATE on a schema), a member of the owner, a role that owns nothing and
-- roles that only read or write. The event triggers of the extension fire for every one of them on each DROP and ALTER TABLE: 3.2.0 and 3.2.1
-- refused every DROP of a role that was neither a superuser nor the extension's owner ("permission denied for schema sift").
CREATE EXTENSION pg_sift;
\set VERBOSITY terse
\set ECHO none
\ir test/roles_helpers.sql
\set ECHO all
SELECT rr_roles_drop('rrd_');
CREATE ROLE rrd_owner;
CREATE ROLE rrd_member IN ROLE rrd_owner;
CREATE ROLE rrd_other;
CREATE ROLE rrd_writer;
CREATE ROLE rrd_reader;
CREATE ROLE rrd_none;
CREATE SCHEMA rrd;
CREATE SCHEMA rrd2;
GRANT USAGE ON SCHEMA rrd, rrd2 TO rrd_writer, rrd_reader, rrd_none;
GRANT USAGE, CREATE ON SCHEMA rrd, rrd2 TO rrd_owner, rrd_member, rrd_other;
SELECT current_database() AS db \gset
GRANT CREATE ON DATABASE :"db" TO rrd_owner;
SET enable_seqscan = off;

-- the owner: plain objects and the three kinds of sift objects, created, changed, moved and dropped
SELECT * FROM rr_run($$
rrd_owner | CREATE TABLE rrd.plain (id int, body text)
rrd_owner | CREATE VIEW rrd.plain_v AS SELECT id FROM rrd.plain
rrd_owner | CREATE TABLE rrd.docs (id int PRIMARY KEY, body text, g int, v float8, doc jsonb)
rrd_owner | INSERT INTO rrd.docs SELECT i, 'alpha beta ' || i, i % 3, i, '{"a": 1}' FROM generate_series(1, 300) i
rrd_owner | CREATE INDEX docs_idx ON rrd.docs USING sift (body)
rrd_owner | CREATE INDEX docs_col ON rrd.docs USING sift_col (id, g, v)
rrd_owner | REINDEX INDEX rrd.docs_idx
rrd_owner | REINDEX INDEX rrd.docs_col
rrd_owner | REINDEX TABLE rrd.docs
rrd_owner | ALTER INDEX rrd.docs_idx RENAME TO docs_bm25
rrd_owner | ALTER TABLE rrd.docs ADD COLUMN extra int
rrd_owner | ALTER TABLE rrd.docs ALTER COLUMN extra TYPE bigint
rrd_owner | ALTER TABLE rrd.docs RENAME COLUMN extra TO extra2
rrd_owner | ALTER TABLE rrd.docs DROP COLUMN extra2
rrd_owner | ALTER TABLE rrd.docs ALTER COLUMN v TYPE float4
rrd_owner | ALTER TABLE rrd.docs ALTER COLUMN body TYPE varchar
rrd_owner | ALTER TABLE rrd.docs SET TABLESPACE pg_default
rrd_owner | SELECT count(*) FROM rrd.docs WHERE body @@> 'alpha'
rrd_owner | ALTER TABLE rrd.docs SET SCHEMA rrd2
rrd_owner | ALTER TABLE rrd2.docs SET ACCESS METHOD sift_table
rrd_owner | DROP INDEX rrd2.docs_col
rrd_owner | ALTER TABLE rrd2.docs SET ACCESS METHOD sift_table
rrd_owner | ALTER TABLE rrd2.docs ALTER COLUMN doc TYPE json USING doc::json
rrd_owner | ALTER TABLE rrd2.docs ADD COLUMN extra int
rrd_owner | ALTER TABLE rrd2.docs SET ACCESS METHOD heap
rrd_owner | ALTER TABLE rrd2.docs OWNER TO rrd_other
rrd_owner | TRUNCATE rrd2.docs
rrd_owner | DROP INDEX rrd2.docs_bm25
rrd_owner | DROP TABLE rrd2.docs
rrd_owner | DROP VIEW rrd.plain_v
rrd_owner | DROP TABLE rrd.plain
$$);

-- a role that is not the owner: every statement that changes the table or its indexes is refused with the usual error, never one about the
-- sift schema or its tables
SELECT * FROM rr_run($$
rrd_owner | CREATE TABLE rrd.docs (id int PRIMARY KEY, body text, g int, v float8, doc jsonb)
rrd_owner | INSERT INTO rrd.docs SELECT i, 'alpha beta ' || i, i % 3, i, '{"a": 1}' FROM generate_series(1, 300) i
rrd_owner | CREATE INDEX docs_idx ON rrd.docs USING sift (body)
rrd_owner | CREATE INDEX docs_col ON rrd.docs USING sift_col (id, g, v)
rrd_owner | CREATE TABLE rrd.facts (id int, g int, val float8, doc jsonb) USING sift_table
rrd_owner | INSERT INTO rrd.facts SELECT i, i % 3, i, '{"a": 1}' FROM generate_series(1, 300) i
rrd_owner | GRANT SELECT ON rrd.docs, rrd.facts TO rrd_reader, rrd_writer
rrd_owner | GRANT INSERT, UPDATE, DELETE ON rrd.docs, rrd.facts TO rrd_writer
$$);
SELECT * FROM rr_matrix(ARRAY['rrd_other', 'rrd_writer', 'rrd_reader', 'rrd_none'], $$
CREATE INDEX x_idx ON rrd.docs USING sift (body)
CREATE INDEX x_col ON rrd.docs USING sift_col (g)
CREATE INDEX x_btree ON rrd.facts (g)
DROP INDEX rrd.docs_idx
DROP INDEX rrd.docs_col
REINDEX INDEX rrd.docs_idx
REINDEX INDEX rrd.docs_col
REINDEX TABLE rrd.facts
ALTER INDEX rrd.docs_idx RENAME TO x
ALTER TABLE rrd.docs ADD COLUMN z int
ALTER TABLE rrd.facts ADD COLUMN z int
ALTER TABLE rrd.docs ALTER COLUMN v TYPE float4
ALTER TABLE rrd.docs SET ACCESS METHOD heap
ALTER TABLE rrd.facts SET ACCESS METHOD heap
ALTER TABLE rrd.docs SET SCHEMA rrd2
ALTER TABLE rrd.docs RENAME TO x
ALTER TABLE rrd.docs OWNER TO rrd_other
TRUNCATE rrd.docs
TRUNCATE rrd.facts
DROP TABLE rrd.docs
DROP TABLE rrd.facts
DROP TABLE rrd.docs, rrd.facts
DROP SCHEMA rrd CASCADE
DROP OWNED BY rrd_owner
REASSIGN OWNED BY rrd_owner TO rrd_other
$$);
SELECT count(*) AS docs_left FROM rrd.docs;
SELECT count(*) AS facts_left FROM rrd.facts;

-- a member of the owner role has the owner's rights; what it creates is its own
SELECT * FROM rr_run($$
rrd_member | CREATE INDEX docs_idx2 ON rrd.docs USING sift (body)
rrd_member | REINDEX INDEX rrd.docs_idx2
rrd_member | DROP INDEX rrd.docs_idx2
rrd_member | ALTER TABLE rrd.docs ADD COLUMN z int
rrd_member | ALTER TABLE rrd.facts ADD COLUMN z int
rrd_member | TRUNCATE rrd.facts
rrd_member | CREATE TABLE rrd.facts_m (id int, val int) USING sift_table
rrd_member | DROP TABLE rrd.facts_m
rrd_member | DROP TABLE rrd.docs, rrd.facts
rrd_other | CREATE TABLE rrd.facts_o (id int, val int) USING sift_table
rrd_other | DROP TABLE rrd.facts_o
$$);
SELECT count(*) AS left_in_rrd FROM pg_class WHERE relnamespace = 'rrd'::regnamespace;

-- schemas and ownership: the owner creates a schema (the database CREATE privilege) and drops it with everything in it; DROP OWNED and
-- REASSIGN OWNED fire the drop trigger for sift tables too
SELECT * FROM rr_run($$
rrd_owner | CREATE SCHEMA rrd_own
rrd_owner | CREATE TABLE rrd_own.docs (id int, body text)
rrd_owner | CREATE INDEX ON rrd_own.docs USING sift (body)
rrd_owner | CREATE INDEX ON rrd_own.docs USING sift_col (id)
rrd_owner | CREATE TABLE rrd_own.facts (id int, val int) USING sift_table
rrd_other | DROP SCHEMA rrd_own CASCADE
rrd_other | DROP SCHEMA rrd_own
rrd_owner | DROP SCHEMA rrd_own
rrd_owner | DROP SCHEMA rrd_own CASCADE
rrd_owner | CREATE TABLE rrd.docs (id int, body text)
rrd_owner | CREATE INDEX ON rrd.docs USING sift (body)
rrd_owner | CREATE TABLE rrd.facts (id int, val int) USING sift_table
rrd_member | REASSIGN OWNED BY rrd_owner TO rrd_member
rrd_member | ALTER TABLE rrd.docs ADD COLUMN z int
rrd_owner | DROP OWNED BY rrd_owner
rrd_member | DROP OWNED BY rrd_member
$$);
SELECT count(*) AS left_in_schemas FROM pg_class WHERE relnamespace IN ('rrd'::regnamespace, 'rrd2'::regnamespace);
SELECT count(*) AS paths_left FROM sift.table_paths;

-- partitioned tables and inheritance, with sift indexes on the partitions and sift_table partitions
SELECT * FROM rr_run($$
rrd_owner | CREATE TABLE rrd.part (id int, body text, g int) PARTITION BY RANGE (id)
rrd_owner | CREATE TABLE rrd.part_1 PARTITION OF rrd.part FOR VALUES FROM (0) TO (100)
rrd_owner | CREATE TABLE rrd.part_2 PARTITION OF rrd.part FOR VALUES FROM (100) TO (200) USING sift_table
rrd_owner | CREATE TABLE rrd.part_3 (id int, body text, g int)
rrd_owner | INSERT INTO rrd.part SELECT i, 'alpha beta ' || i, i % 3 FROM generate_series(0, 199) i
rrd_owner | CREATE INDEX part_idx ON rrd.part USING sift (body)
rrd_owner | CREATE INDEX part_col ON rrd.part USING sift_col (id, g)
rrd_owner | CREATE INDEX part_1_idx ON rrd.part_1 USING sift (body)
rrd_owner | CREATE INDEX part_3_idx ON rrd.part_3 USING sift (body)
rrd_owner | ALTER TABLE rrd.part ATTACH PARTITION rrd.part_3 FOR VALUES FROM (200) TO (300)
rrd_owner | ALTER TABLE rrd.part DETACH PARTITION rrd.part_3
rrd_owner | ALTER TABLE rrd.part ADD COLUMN extra int
rrd_owner | GRANT SELECT ON rrd.part, rrd.part_1, rrd.part_2, rrd.part_3 TO rrd_reader
rrd_reader | SELECT count(*) FROM rrd.part WHERE body @@> 'alpha'
rrd_reader | SELECT count(*) FROM rrd.part_1 WHERE body @@> 'alpha'
rrd_reader | SELECT count(*), sum(id) FROM rrd.part GROUP BY g ORDER BY g
rrd_reader | DROP TABLE rrd.part_1
rrd_owner | DROP TABLE rrd.part_3
rrd_owner | DROP TABLE rrd.part_2
rrd_owner | DROP TABLE rrd.part
rrd_owner | CREATE TABLE rrd.base (id int, body text)
rrd_owner | CREATE TABLE rrd.child (extra int) INHERITS (rrd.base)
rrd_owner | CREATE INDEX base_idx ON rrd.base USING sift (body)
rrd_owner | CREATE INDEX child_idx ON rrd.child USING sift (body)
rrd_owner | INSERT INTO rrd.child SELECT i, 'alpha ' || i, 1 FROM generate_series(1, 50) i
rrd_owner | ALTER TABLE rrd.child NO INHERIT rrd.base
rrd_owner | ALTER TABLE rrd.child INHERIT rrd.base
rrd_owner | DROP TABLE rrd.child
rrd_owner | DROP TABLE rrd.base
$$);

-- ordinary objects in a database where the extension is installed: the triggers must not get in the way
SELECT * FROM rr_run($$
rrd_owner | CREATE TABLE rrd.t (id int PRIMARY KEY, val int, tags text[])
rrd_owner | CREATE TABLE rrd.t_copy AS SELECT * FROM rrd.t
rrd_owner | CREATE MATERIALIZED VIEW rrd.mv AS SELECT val, count(*) FROM rrd.t GROUP BY val
rrd_owner | CREATE SEQUENCE rrd.s
rrd_owner | CREATE TYPE rrd.ty AS (a int, b text)
rrd_owner | CREATE DOMAIN rrd.dom AS int CHECK (VALUE > 0)
rrd_owner | CREATE FUNCTION rrd.f(int) RETURNS int LANGUAGE sql AS 'SELECT $1 + 1'
rrd_owner | CREATE INDEX t_val ON rrd.t (val)
rrd_owner | CREATE INDEX t_tags ON rrd.t USING gin (tags)
rrd_owner | ALTER TABLE rrd.t ADD COLUMN extra text DEFAULT 'x'
rrd_owner | ALTER TABLE rrd.t ADD CONSTRAINT t_chk CHECK (val >= 0)
rrd_owner | ALTER TABLE rrd.t ALTER COLUMN id TYPE bigint
rrd_owner | ALTER TABLE rrd.t RENAME COLUMN extra TO extra2
rrd_owner | ALTER TABLE rrd.t DROP COLUMN extra2
rrd_owner | ALTER TABLE rrd.t SET UNLOGGED
rrd_owner | ALTER TABLE rrd.t SET LOGGED
rrd_owner | ALTER TABLE rrd.t SET (fillfactor = 70)
rrd_owner | COMMENT ON TABLE rrd.t IS 'plain'
rrd_owner | GRANT SELECT ON rrd.t TO rrd_reader
rrd_owner | REVOKE SELECT ON rrd.t FROM rrd_reader
rrd_owner | ALTER TABLE rrd.t RENAME TO t2
rrd_owner | ALTER TABLE rrd.t2 SET SCHEMA rrd2
rrd_owner | DROP INDEX rrd2.t_val
rrd_owner | DROP MATERIALIZED VIEW rrd.mv
rrd_owner | DROP TABLE rrd.t_copy, rrd2.t2
rrd_owner | DROP FUNCTION rrd.f(int)
rrd_owner | DROP DOMAIN rrd.dom
rrd_owner | DROP TYPE rrd.ty
rrd_owner | DROP SEQUENCE rrd.s
rrd_reader | CREATE TABLE rrd.r (id int)
rrd_owner | CREATE TABLE rrd.t (id int)
rrd_reader | ALTER TABLE rrd.t ADD COLUMN x int
rrd_reader | DROP TABLE rrd.t
rrd_owner | DROP TABLE rrd.t
$$);

-- CONCURRENTLY runs outside a transaction block: the owner, then a role that is not the owner
SET SESSION AUTHORIZATION rrd_owner;
CREATE TABLE rrd.conc (id int, body text, g int, v float8);
INSERT INTO rrd.conc SELECT i, 'alpha ' || i, i % 3, i FROM generate_series(1, 300) i;
CREATE INDEX CONCURRENTLY conc_btree ON rrd.conc (g);
CREATE INDEX CONCURRENTLY conc_idx ON rrd.conc USING sift (body);
CREATE INDEX CONCURRENTLY conc_col ON rrd.conc USING sift_col (id, g, v);
REINDEX INDEX CONCURRENTLY rrd.conc_idx;
REINDEX INDEX CONCURRENTLY rrd.conc_col;
REINDEX TABLE CONCURRENTLY rrd.conc;
SELECT count(*) FROM rrd.conc WHERE body @@> 'alpha';
RESET SESSION AUTHORIZATION;
SET SESSION AUTHORIZATION rrd_other;
CREATE INDEX CONCURRENTLY conc_x ON rrd.conc USING sift (body);
REINDEX INDEX CONCURRENTLY rrd.conc_idx;
REINDEX TABLE CONCURRENTLY rrd.conc;
DROP INDEX CONCURRENTLY rrd.conc_idx;
RESET SESSION AUTHORIZATION;
SET SESSION AUTHORIZATION rrd_member;
DROP INDEX CONCURRENTLY rrd.conc_idx;
DROP INDEX CONCURRENTLY rrd.conc_col;
DROP TABLE rrd.conc;
RESET SESSION AUTHORIZATION;

DROP SCHEMA rrd, rrd2 CASCADE;
DROP FUNCTION rr_run(text), rr_matrix(text[], text), rr_roles_drop(text);
DROP OWNED BY rrd_owner, rrd_member, rrd_other, rrd_writer, rrd_reader, rrd_none;
DROP ROLE rrd_owner, rrd_member, rrd_other, rrd_writer, rrd_reader, rrd_none;
DROP EXTENSION pg_sift;
