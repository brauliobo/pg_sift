-- The statements the server runs on the catalogs of the extension as their owner (the TTL and tier policies, the per-table options) resolve every name in pg_catalog: an operator a table
-- owner puts first in the search_path of the session never runs with the rights of the owner of the catalogs. The operator below makes its role a superuser when it runs as one; the row of
-- the TTL of another table is what it is called on.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse
CREATE ROLE sp_owner;
CREATE SCHEMA sp AUTHORIZATION sp_owner;
CREATE TABLE events (id int, created_at timestamp NOT NULL) USING sift_table;
SELECT sift.set_ttl('events', 'created_at', interval '30 days') AS ttl_row;

SET ROLE sp_owner;
CREATE FUNCTION sp.oideq(oid, oid) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
	IF (SELECT rolsuper FROM pg_catalog.pg_roles WHERE rolname = current_user) THEN
		EXECUTE 'ALTER ROLE sp_owner SUPERUSER';
	END IF;
	RETURN $1 OPERATOR(pg_catalog.=) $2;
END $$;
CREATE OPERATOR sp.= (FUNCTION = sp.oideq, LEFTARG = oid, RIGHTARG = oid);
CREATE TABLE sp.facts (id int, created_at timestamp NOT NULL, value_num int) USING sift_table;
INSERT INTO sp.facts SELECT g, timestamp '2020-01-01' + g * interval '1 hour', g FROM generate_series(1, 10) g;

-- the read of the TTL of the table before a column is dropped
SET search_path = sp, pg_catalog;
ALTER TABLE sp.facts DROP COLUMN value_num;
RESET search_path;
RESET ROLE;
SELECT rolsuper AS superuser_after_drop_column FROM pg_roles WHERE rolname = 'sp_owner';
ALTER ROLE sp_owner NOSUPERUSER;

-- the deletes of the rows of a table that leaves sift_table
SET ROLE sp_owner;
SET search_path = sp, pg_catalog;
ALTER TABLE sp.facts SET ACCESS METHOD heap;
RESET search_path;
RESET ROLE;
SELECT rolsuper AS superuser_after_set_access_method FROM pg_roles WHERE rolname = 'sp_owner';
SELECT count(*) AS rows_kept FROM sp.facts;
SELECT count(*) AS ttl_rows FROM sift.ttl_policies;

ALTER ROLE sp_owner NOSUPERUSER;

-- the refresh of the pairs of a followed table, as the owner of the pairs: the view of a role can neither change the search_path of the session that wrote the table nor make a
-- temporary table that a later statement of that session reads
SET ROLE sp_owner;
CREATE TABLE sp.src (member int, tenant int);
CREATE TABLE sp.pairs (member int, tenant int, UNIQUE (member, tenant));
CREATE FUNCTION sp.plant(int) RETURNS int LANGUAGE plpgsql AS $$
BEGIN
	PERFORM pg_catalog.set_config('search_path', 'sp, pg_catalog', false);
	RETURN $1;
END $$;
CREATE FUNCTION sp.plant_temp(int) RETURNS int LANGUAGE plpgsql AS $$
BEGIN
	CREATE TEMP TABLE IF NOT EXISTS pg_roles (rolname name, rolsuper bool);
	RETURN $1;
END $$;
CREATE VIEW sp.pairs_v AS SELECT sp.plant(member) AS member, tenant FROM sp.src;
SELECT * FROM sift.scope_follow_pairs('sp.pairs', 'sp.pairs_v', 'sp.src', 'member', 'member');
RESET search_path;
RESET ROLE;
INSERT INTO sp.src VALUES (1, 1);
SHOW search_path;
SELECT 1::oid = 1::oid AS planted_operator;
RESET search_path;
SELECT rolsuper AS superuser_after_follow_refresh FROM pg_roles WHERE rolname = 'sp_owner';
ALTER ROLE sp_owner NOSUPERUSER;
SET ROLE sp_owner;
CREATE OR REPLACE VIEW sp.pairs_v AS SELECT sp.plant_temp(member) AS member, tenant FROM sp.src;
RESET ROLE;
INSERT INTO sp.src VALUES (2, 2);
SELECT count(*) AS planted_temp_tables FROM pg_class WHERE relpersistence = 't' AND relname = 'pg_roles';
SELECT count(*) AS pairs FROM sp.pairs;
SET ROLE sp_owner;
SELECT sift.scope_unfollow_pairs('sp.pairs');
RESET ROLE;

ALTER ROLE sp_owner NOSUPERUSER;
DROP SCHEMA sp CASCADE;
DROP TABLE events;
DROP ROLE sp_owner;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
