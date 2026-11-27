-- A role with no right runs no code as the owner of a table through the projections: sift.projection_filter_served is for the definer functions of the extension only (it folds a
-- filter with the rights of the owner of the table and takes sift_col indexes only), and sift._projection_get reads and locks a definition for the owner of its table only.
CREATE EXTENSION pg_sift;
\set VERBOSITY terse
SET client_min_messages = warning;
DROP ROLE IF EXISTS s114_atk, s114_own, s114_read;
CREATE ROLE s114_atk;
CREATE ROLE s114_own;
CREATE ROLE s114_read;
GRANT USAGE ON SCHEMA sift TO s114_atk, s114_own, s114_read;
GRANT CREATE ON SCHEMA public TO s114_own;

-- a function of the caller that a superuser fold would run with the rights of the bootstrap superuser
SET ROLE s114_atk;
CREATE FUNCTION pg_temp.leak_secret() RETURNS boolean LANGUAGE plpgsql IMMUTABLE AS $$
BEGIN
	RAISE EXCEPTION 'PWNED as %, % roles read from pg_authid', current_user, (SELECT count(*) > 0 FROM pg_authid);
END $$;
SELECT sift.projection_filter_served('pg_catalog.pg_class'::regclass, 'pg_temp.leak_secret()', 'pg_catalog.pg_class_oid_index'::regclass);
SELECT sift.projection_filter_served('pg_catalog.pg_class'::regclass, 'oid > 0', 'pg_catalog.pg_class_oid_index'::regclass);
DROP FUNCTION pg_temp.leak_secret();
RESET ROLE;

-- a superuser may call it, with sift_col indexes of the table only, and the search_path of a maintenance command
SELECT sift.projection_filter_served('pg_catalog.pg_class'::regclass, 'oid > 0', 'pg_catalog.pg_class_oid_index'::regclass);
CREATE TABLE s114_t (id int, kind int);
CREATE INDEX s114_ix ON s114_t USING sift_col (id, kind);
SELECT sift.projection_filter_served('s114_t'::regclass, 'kind = 1', 's114_ix'::regclass) AS served;
CREATE FUNCTION public.s114_true() RETURNS boolean LANGUAGE sql IMMUTABLE RETURN true;
SELECT sift.projection_filter_served('s114_t'::regclass, 's114_true()', 's114_ix'::regclass) AS unqualified;
SELECT sift.projection_filter_served('s114_t'::regclass, 'public.s114_true()', 's114_ix'::regclass) AS qualified;
DROP TABLE s114_t;
DROP FUNCTION public.s114_true();

-- a projection of a table of another role: a role that may not look at the table reads no definition, one that may look at it locks none, the owner changes it
SET ROLE s114_own;
CREATE TABLE s114_secret (id int, kind int, note text);
SELECT sift.projection_create('s114_p', 's114_secret', ARRAY['id', 'kind'], filter => 'kind = 3');
GRANT SELECT ON s114_secret TO s114_read;
RESET ROLE;
SET ROLE s114_atk;
SELECT (sift._projection_get('s114_p')).filter AS leaked;
SELECT count(*) AS listed FROM sift.projections;
RESET ROLE;
SET ROLE s114_read;
SELECT (sift._projection_get('s114_p')).filter AS locked;
SELECT sift.projection_replace('s114_p', ARRAY['id'], filter => 'kind = 4');
RESET ROLE;
SET ROLE s114_own;
SELECT (sift._projection_get('s114_p')).filter AS own;
SELECT sift.projection_replace('s114_p', ARRAY['id'], filter => 'kind = 4');
SELECT (sift._projection_get('s114_p')).filter AS replaced;
SELECT sift.projection_drop('s114_p');
DROP TABLE s114_secret;
RESET ROLE;

REVOKE ALL ON SCHEMA public FROM s114_own;
REVOKE ALL ON SCHEMA sift FROM s114_atk, s114_own, s114_read;
DROP ROLE s114_atk, s114_own, s114_read;
DROP EXTENSION pg_sift;
