-- Maintenance and diagnostics as ordinary roles: the entry points a table owner (or a role with MAINTAIN) calls work for them and are refused
-- to everyone else with the error of the object, not one about the extension's schema or tables; the diagnostics an ordinary role may use
-- work for it; VACUUM, ANALYZE, CLUSTER and REINDEX follow the usual rules for the three kinds of sift objects.
-- Every role has USAGE on the schema sift (PUBLIC has it) except in the one section that takes it back.
CREATE EXTENSION pg_sift;
\set VERBOSITY terse
\set ECHO none
\ir test/roles_helpers.sql
\set ECHO all
SELECT rr_roles_drop('rm_');
CREATE ROLE rm_own;
CREATE ROLE rm_mem IN ROLE rm_own;
CREATE ROLE rm_mt;
CREATE ROLE rm_wr;
CREATE ROLE rm_rd;
CREATE ROLE rm_non;
CREATE ROLE rm_nos;
CREATE SCHEMA rm;
GRANT USAGE ON SCHEMA rm TO rm_mt, rm_wr, rm_rd, rm_non, rm_nos;
GRANT USAGE, CREATE ON SCHEMA rm TO rm_own, rm_mem;
SET enable_seqscan = off;

SELECT * FROM rr_run($$
rm_own | CREATE TABLE rm.docs (id int PRIMARY KEY, body text, g int, v float8)
rm_own | INSERT INTO rm.docs SELECT i, 'alpha beta ' || i, i % 3, i FROM generate_series(1, 300) i
rm_own | CREATE INDEX docs_idx ON rm.docs USING sift (body)
rm_own | CREATE INDEX docs_col ON rm.docs USING sift_col (id, g, v)
rm_own | CREATE TABLE rm.facts (id int, g int, val float8, doc jsonb) USING sift_table
rm_own | INSERT INTO rm.facts SELECT i, i % 3, i, '{"a": 1}' FROM generate_series(1, 300) i
rm_own | GRANT SELECT ON rm.docs, rm.facts TO rm_mt, rm_wr, rm_rd, rm_nos
rm_own | GRANT INSERT, UPDATE, DELETE ON rm.docs, rm.facts TO rm_wr
rm_own | GRANT MAINTAIN ON rm.docs, rm.facts TO rm_mt
$$);

-- the entry points of the table's owner, of a member of the owner and of a role with MAINTAIN; no other role, whatever it may read or write
SELECT * FROM rr_matrix(ARRAY['rm_own', 'rm_mem', 'rm_mt', 'rm_wr', 'rm_rd', 'rm_non', 'rm_nos'], $$
!SELECT sift.promote('rm.docs_idx')
!SELECT sift.merge('rm.docs_idx')
!SELECT sift.merge('rm.docs_idx', true)
!SELECT sift.verify('rm.docs_idx', true)
!SELECT * FROM sift.segments('rm.docs_idx')
!SELECT * FROM sift.segment_postings('rm.docs_idx')
!SELECT * FROM sift.index_info('rm.docs_idx')
!SELECT sift.col_promote('rm.docs_col')
!SELECT sift.col_merge('rm.docs_col')
!SELECT sift.col_merge('rm.docs_col', true)
!SELECT sift.col_maintain('rm.docs_col')
!SELECT sift.col_refresh('rm.docs_col', 1)
!SELECT sift.col_rollup_refresh('rm.docs_col')
!SELECT sift.table_maintain('rm.facts')
!SELECT sift.table_set_paths('rm.facts', 'doc', 'doc->a int4')
$$);

-- the diagnostics: counters of the session, the indexes the role can read, why a query was not pushed down; what an update leaves to do and the
-- declared paths are listed for the tables the role owns or may maintain, and for no other
SELECT * FROM rr_matrix(ARRAY['rm_own', 'rm_mem', 'rm_mt', 'rm_wr', 'rm_rd', 'rm_non', 'rm_nos'], $$
SELECT sift.version()
!SELECT * FROM sift.stats()
!SELECT sift.stats_reset()
!SELECT * FROM sift.refusal_slugs()
SELECT object::text, kind FROM sift.upgrade_commands() ORDER BY 1
!SELECT * FROM sift.col_rollup_counters()
!SELECT * FROM sift.col_rollup_queue()
!SELECT * FROM sift.col_rollup_misses()
SELECT index::text, kind FROM sift.workers ORDER BY 1
!SELECT * FROM sift.col_rollup_state
!SELECT * FROM sift.col_rollup_requests
!SELECT * FROM sift.explain_why('SELECT g, count(*) FROM rm.docs GROUP BY g')
!SELECT * FROM sift.explain_why('SELECT g, count(*) FROM rm.facts GROUP BY g')
!SELECT * FROM sift.worker_state('rm.docs_idx')
!SELECT * FROM sift.col_maintenance_due('rm.docs_col')
!SELECT * FROM sift.col_index_info('rm.docs_col')
!SELECT * FROM sift.col_segments('rm.docs_col')
!SELECT * FROM sift.col_leaves('rm.docs_col')
!SELECT * FROM sift.col_sections('rm.docs_col')
!SELECT * FROM sift.col_clustering('rm.docs_col')
!SELECT * FROM sift.col_unit_agg('rm.docs_col', 'id')
!SELECT * FROM sift.col_unit_hist('rm.docs_col', 'g')
!SELECT sift.col_verify('rm.docs_col', true)
!SELECT * FROM sift.table_segments('rm.facts')
!SELECT * FROM sift.table_space('rm.facts')
!SELECT * FROM sift.col_sections('rm.facts')
!SELECT * FROM sift.table_paths
SELECT relid::text, attname FROM sift.table_paths_maintained ORDER BY 1
!SELECT sift.tokenize('Hello World')
!SELECT sift.parse_query('alpha beta')
!SELECT sift.highlight('alpha beta', 'alpha')
$$);

-- with USAGE on the schema sift taken back from PUBLIC and given to the owner: every other role is refused the schema, not an object of it
REVOKE USAGE ON SCHEMA sift FROM PUBLIC;
GRANT USAGE ON SCHEMA sift TO rm_own;
SELECT * FROM rr_matrix(ARRAY['rm_own', 'rm_rd', 'rm_nos'], $$
SELECT sift.version()
!SELECT sift.promote('rm.docs_idx')
!SELECT * FROM sift.stats()
!SELECT * FROM sift.workers
$$);
REVOKE USAGE ON SCHEMA sift FROM rm_own;
GRANT USAGE ON SCHEMA sift TO PUBLIC;

-- what a role that owns and may read nothing can do with the relations of the schema sift: read the four views and nothing else
SELECT c.relname, c.relkind, has_table_privilege('rm_nos', c.oid, 'SELECT') AS can_select
	FROM pg_class c WHERE c.relnamespace = 'sift'::regnamespace AND c.relkind IN ('r', 'v', 'm', 'S', 'p', 'f')
	  AND has_table_privilege('rm_nos', c.oid, 'SELECT,INSERT,UPDATE,DELETE,TRUNCATE,REFERENCES,TRIGGER') ORDER BY 1;
SELECT has_schema_privilege('rm_nos', 'sift', 'USAGE') AS usage, has_schema_privilege('rm_nos', 'sift', 'CREATE') AS create_in_sift;
SELECT count(*) AS other_privileges FROM pg_class c WHERE c.relnamespace = 'sift'::regnamespace AND c.relkind IN ('r', 'v', 'm', 'S', 'p', 'f')
	AND has_table_privilege('rm_nos', c.oid, 'INSERT,UPDATE,DELETE,TRUNCATE,REFERENCES,TRIGGER');

-- VACUUM, ANALYZE, CLUSTER and REINDEX: the owner and a role with MAINTAIN may; any other role is skipped with a warning (VACUUM, ANALYZE) or refused
SET SESSION AUTHORIZATION rm_own;
VACUUM rm.docs;
VACUUM (ANALYZE) rm.docs;
VACUUM FULL rm.docs;
ANALYZE rm.docs;
VACUUM rm.facts;
VACUUM (ANALYZE) rm.facts;
VACUUM FULL rm.facts;
ANALYZE rm.facts;
CLUSTER rm.docs USING docs_pkey;
REINDEX TABLE rm.docs;
REINDEX TABLE rm.facts;
SELECT count(*) FROM rm.docs WHERE body @@> 'alpha';
SELECT g, count(*) FROM rm.facts GROUP BY g ORDER BY g;
RESET SESSION AUTHORIZATION;
SET SESSION AUTHORIZATION rm_mt;
VACUUM rm.docs;
VACUUM FULL rm.docs;
ANALYZE rm.docs;
VACUUM FULL rm.facts;
ANALYZE rm.facts;
CLUSTER rm.docs USING docs_pkey;
REINDEX TABLE rm.docs;
REINDEX TABLE rm.facts;
SELECT count(*) FROM rm.docs WHERE body @@> 'alpha';
RESET SESSION AUTHORIZATION;
SET SESSION AUTHORIZATION rm_wr;
VACUUM rm.docs;
VACUUM FULL rm.facts;
ANALYZE rm.docs;
ANALYZE rm.facts;
CLUSTER rm.docs USING docs_pkey;
REINDEX TABLE rm.docs;
REINDEX INDEX rm.docs_col;
RESET SESSION AUTHORIZATION;
SET SESSION AUTHORIZATION rm_non;
VACUUM rm.docs;
ANALYZE rm.facts;
RESET SESSION AUTHORIZATION;

DROP SCHEMA rm CASCADE;
DROP FUNCTION rr_run(text), rr_matrix(text[], text), rr_pushdown_diff(text[], text), rr_script(text[], text), rr_roles_drop(text);
DROP OWNED BY rm_own, rm_mem, rm_mt, rm_wr, rm_rd, rm_non, rm_nos;
DROP ROLE rm_own, rm_mem, rm_mt, rm_wr, rm_rd, rm_non, rm_nos;
DROP EXTENSION pg_sift;
