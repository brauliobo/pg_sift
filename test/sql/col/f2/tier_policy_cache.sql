-- A session keeps the backend of each tier policy it opened, by policy id. DROP EXTENSION and CREATE EXTENSION start the ids again at 1: the table of the new extension that gets id 1 reads and
-- writes the store of its own policy, not the one the session opened for the policy that had that id before.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse
SELECT current_setting('data_directory') || '/p98_tier_store_a' AS store_a \gset
SELECT current_setting('data_directory') || '/p98_tier_store_b' AS store_b \gset
\set mk 'cat >/dev/null && rm -rf ' :store_a ' ' :store_b ' && mkdir -p ' :store_a ' ' :store_b
COPY (SELECT 1) TO PROGRAM :'mk';

CREATE TABLE ev (id int8, ts timestamp NOT NULL, v int, pad text) USING sift_table;
INSERT INTO ev SELECT i, timestamp '2020-01-01' + i * interval '1 hour', i % 100, md5(i::text) FROM generate_series(1, 5000) i;
VACUUM (FREEZE) ev;
SELECT sift.set_tier_policy('ev', 'file://' || :'store_a', '30 days') AS policy_a;
VACUUM ev;
SELECT policy_id, count(*) OVER () AS policies FROM sift.tier_policies;
SELECT count(*) AS tiered FROM sift.tier_status;
SELECT count(*) AS rows, sum(v) AS sum_v FROM ev;

DROP EXTENSION pg_sift CASCADE;
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
CREATE TABLE ev2 (id int8, ts timestamp NOT NULL, v int, pad text) USING sift_table;
INSERT INTO ev2 SELECT i, timestamp '2020-01-01' + i * interval '1 hour', i % 100, md5(i::text) FROM generate_series(1, 5000) i;
VACUUM (FREEZE) ev2;
SELECT sift.set_tier_policy('ev2', 'file://' || :'store_b', '30 days') AS policy_b;
VACUUM ev2;
SELECT policy_id, count(*) OVER () AS policies FROM sift.tier_policies;
SELECT count(*) AS tiered FROM sift.tier_status;
SELECT count(*) AS rows, sum(v) AS sum_v FROM ev2;
SELECT count(*) AS problems FROM sift.check('ev2');
SELECT count(*) > 0 AS store_b_has_objects FROM pg_ls_dir(:'store_b');

DROP TABLE ev2;
\set rm 'cat >/dev/null && rm -rf ' :store_a ' ' :store_b
COPY (SELECT 1) TO PROGRAM :'rm';
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
