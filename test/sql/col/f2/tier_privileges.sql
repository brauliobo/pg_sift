-- The tier policy names server resources (a directory of the server host, an object store with the credentials of the server environment): only a superuser sets or clears it, as only a
-- superuser makes a tablespace. A table owner, who holds MAINTAIN, is refused before any directory is made or any connection is tried.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse
SELECT current_setting('data_directory') || '/p98_tier_escape' AS escdir \gset
SELECT current_setting('data_directory') || '/p98_tier_granted' AS okdir \gset
\set mk 'cat >/dev/null && rm -rf ' :escdir ' ' :okdir
COPY (SELECT 1) TO PROGRAM :'mk';
CREATE ROLE p98_tier_owner;
CREATE TABLE ev (id int8, ts timestamp NOT NULL, pad text) USING sift_table;
INSERT INTO ev SELECT i, timestamp '2020-01-01' + i * interval '1 hour', md5(i::text) FROM generate_series(1, 200) i;
ALTER TABLE ev OWNER TO p98_tier_owner;
CREATE FUNCTION p98_try(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN EXECUTE q; RETURN 'allowed'; EXCEPTION WHEN OTHERS THEN RETURN SQLSTATE; END $$;

SET ROLE p98_tier_owner;
SELECT p98_try('SELECT sift.set_tier_policy(''ev'', ''file://' || :'escdir' || ''')') AS owner_file_policy;
SELECT p98_try($$SELECT sift.set_tier_policy('ev', 's3://bucket/prefix', interval '1 day', 'http://127.0.0.1:1')$$) AS owner_endpoint_policy;
SELECT p98_try($$SELECT sift.clear_tier_policy('ev')$$) AS owner_clear_policy;
SELECT sift.set_ttl('ev', 'ts', interval '30 days') AS owner_ttl;
RESET ROLE;
SELECT (pg_stat_file(:'escdir', true)).size IS NULL AS directory_not_made;
SELECT count(*) AS policies FROM sift.tier_policies;

SELECT sift.set_tier_policy('ev', 'file://' || :'okdir') AS superuser_policy;
SELECT (pg_stat_file(:'okdir', true)).isdir AS directory_made;
SET ROLE p98_tier_owner;
SELECT p98_try($$SELECT sift.clear_tier_policy('ev')$$) AS owner_clear_existing;
RESET ROLE;
SELECT count(*) AS policies FROM sift.tier_policies;
SELECT sift.clear_tier_policy('ev') AS superuser_cleared;

DROP FUNCTION p98_try(text);
DROP TABLE ev;
DROP ROLE p98_tier_owner;
\set rm 'cat >/dev/null && rm -rf ' :okdir
COPY (SELECT 1) TO PROGRAM :'rm';
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
