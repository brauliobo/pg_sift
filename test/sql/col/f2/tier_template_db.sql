-- A database made by CREATE DATABASE ... TEMPLATE has the tier policy, the relations and the tier directories of the original, with the same object names: it reads the objects it was
-- copied with and puts, marks or deletes none, so that a part the original tiers after the copy is neither overwritten by a part of the copy nor marked an orphan by its sweep.
SET client_min_messages = warning;
\set VERBOSITY terse
SELECT current_database() AS home \gset
SELECT current_setting('data_directory') || '/s116_tier_template' AS tierdir \gset
\set mk 'cat >/dev/null && rm -rf ' :tierdir ' && mkdir -p ' :tierdir
COPY (SELECT 1) TO PROGRAM :'mk';
CREATE DATABASE s116_tier_src;
\c s116_tier_src
SET client_min_messages = warning;
\set VERBOSITY terse
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
CREATE TABLE ev (id int8, ts timestamp NOT NULL, v int, pad text) USING sift_table;
INSERT INTO ev SELECT i, timestamp '2020-01-01' + i * interval '1 hour', i % 100, md5(i::text) FROM generate_series(1, 5000) i;
VACUUM (FREEZE) ev;
SELECT sift.set_tier_policy('ev', 'file://' || :'tierdir', '30 days') AS policy;
VACUUM ev;
SELECT count(*) AS src_tiered_before_copy FROM sift.tier_status;
\c :home
CREATE DATABASE s116_tier_copy TEMPLATE s116_tier_src;

\c s116_tier_src
SET client_min_messages = warning;
SET sift.test_create_format1 = off;
INSERT INTO ev SELECT i, timestamp '2020-06-01' + i * interval '1 hour', i % 7, md5(i::text) FROM generate_series(5001, 10000) i;
VACUUM (FREEZE) ev;
VACUUM ev;
SELECT count(*) AS src_tiered FROM sift.tier_status;
SELECT lpad(to_hex(scope_id), 16, '0') AS scope FROM sift.tier_policies \gset

\c s116_tier_copy
SET client_min_messages = warning;
\set VERBOSITY terse
SET sift.test_create_format1 = off;
SELECT count(*) AS copy_rows, sum(v) AS copy_sum FROM ev;
INSERT INTO ev SELECT i, timestamp '2020-06-01' + i * interval '1 hour', i % 11, md5(i::text) FROM generate_series(5001, 10000) i;
VACUUM (FREEZE) ev;
VACUUM ev;
SELECT count(*) AS copy_tiered FROM sift.tier_status;
SELECT count(*) AS copy_rows, sum(v) AS copy_sum FROM ev;

\c s116_tier_src
SET client_min_messages = warning;
\set VERBOSITY terse
SELECT count(*) FILTER (WHERE f LIKE '%.orphan-%') AS orphan_marks, count(*) FILTER (WHERE f LIKE '%.siftpart') AS objects
FROM pg_ls_dir(:'tierdir' || '/r' || :'scope') f;
SELECT count(*) AS src_rows, sum(v) AS src_sum FROM ev;
SELECT count(*) AS src_tiered_after FROM sift.tier_status;

\c :home
DROP DATABASE s116_tier_copy;
DROP DATABASE s116_tier_src;
\set rm 'cat >/dev/null && rm -rf ' :tierdir
COPY (SELECT 1) TO PROGRAM :'rm';
