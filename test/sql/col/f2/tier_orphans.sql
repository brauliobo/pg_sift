-- The objects of a relation that was rewritten (TRUNCATE, VACUUM FULL) are not deleted by the next maintenance: a standby that still replays the rewrite and a recovery to a time before it
-- read them. They are kept for the standby-safe grace, and a policy cannot be cleared while they are, so that no object is abandoned in the store.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse
SELECT current_setting('data_directory') || '/p98_tier_orphans' AS tierdir \gset
\set mk 'cat >/dev/null && rm -rf ' :tierdir ' && mkdir -p ' :tierdir
COPY (SELECT 1) TO PROGRAM :'mk';
CREATE TABLE ev (id int8, ts timestamp NOT NULL, v int, pad text) USING sift_table;
INSERT INTO ev SELECT i, timestamp '2020-01-01' + i * interval '1 hour', i % 100, md5(i::text) FROM generate_series(1, 5000) i;
VACUUM (FREEZE) ev;
SELECT sift.set_tier_policy('ev', 'file://' || :'tierdir', '30 days') AS policy;
VACUUM ev;
SELECT count(*) AS tiered FROM sift.tier_status;
SELECT substr(location, 8) AS objpath FROM sift.tier_status \gset
SELECT (pg_stat_file(:'objpath', true)).size > 0 AS object_there;

TRUNCATE ev;
VACUUM ev;
SELECT count(*) AS tiered FROM sift.tier_status;
SELECT (pg_stat_file(:'objpath', true)).size > 0 AS object_kept_after_truncate;
VACUUM ev;
SELECT (pg_stat_file(:'objpath', true)).size > 0 AS object_kept_after_second_vacuum;

CREATE FUNCTION p98_try(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN EXECUTE q; RETURN 'allowed'; EXCEPTION WHEN OTHERS THEN RETURN SQLSTATE; END $$;
SELECT p98_try($$SELECT sift.clear_tier_policy('ev')$$) AS clear_while_objects_wait;
SELECT count(*) AS policies FROM sift.tier_policies;
SELECT (pg_stat_file(:'objpath', true)).size > 0 AS object_kept_after_refused_clear;

DROP FUNCTION p98_try(text);
DROP TABLE ev;
\set rm 'cat >/dev/null && rm -rf ' :tierdir
COPY (SELECT 1) TO PROGRAM :'rm';
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
