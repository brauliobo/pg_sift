-- ALTER TABLE ... SET ACCESS METHOD heap of a tiered sift_table copies the rows of its parts in object storage: the catalogs of the extension keep the rows of the table
-- (its tier policy) until the rewrite is done, and the table has none of them once it is a heap.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse
SELECT current_setting('data_directory') || '/set_am_tiered' AS tierdir \gset
\set mk 'cat >/dev/null && rm -rf ' :tierdir ' && mkdir -p ' :tierdir
COPY (SELECT 1) TO PROGRAM :'mk';
CREATE TABLE events (id int8, created_at timestamp NOT NULL, value_num int) USING sift_table;
INSERT INTO events SELECT g, timestamp '2020-01-01' + g * interval '1 hour', g % 100 FROM generate_series(1, 5000) g;
VACUUM (FREEZE) events;
SELECT sift.set_tier_policy('events', 'file://' || :'tierdir', '30 days') IS NOT NULL AS policy;
VACUUM events;
SELECT count(*) AS tiered FROM sift.tier_status;
CREATE FUNCTION policies_of_events() RETURNS int8 VOLATILE LANGUAGE sql AS $$SELECT count(*) FROM sift.tier_policies WHERE relid = 'events'::regclass$$;
ALTER TABLE events SET ACCESS METHOD heap, ADD COLUMN policies_seen int8 DEFAULT policies_of_events();
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE c.oid = 'events'::regclass;
SELECT count(*), sum(value_num), min(policies_seen), max(policies_seen) FROM events;
SELECT count(*) AS policies FROM sift.tier_policies WHERE relid = 'events'::regclass;
SELECT count(*) AS quarantined FROM sift.quarantine;
DROP TABLE events;
DROP FUNCTION policies_of_events();
\set rm 'cat >/dev/null && rm -rf ' :tierdir
COPY (SELECT 1) TO PROGRAM :'rm';
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
