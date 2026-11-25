-- A part with wide values (text above the inline limit lives in extents outside the part) is not moved to the store: its object would hold the stream but not the values, and the local blocks
-- would stay. The TTL that drops such a part releases the extents of its values too.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse
SELECT current_setting('data_directory') || '/p98_tier_wide' AS tierdir \gset
\set mk 'cat >/dev/null && rm -rf ' :tierdir ' && mkdir -p ' :tierdir
COPY (SELECT 1) TO PROGRAM :'mk';
CREATE TABLE wide (id int8, ts timestamp NOT NULL, body text) USING sift_table;
CREATE TABLE narrow (id int8, ts timestamp NOT NULL, body text) USING sift_table;
INSERT INTO wide SELECT i, timestamp '2020-01-01' + i * interval '1 hour', repeat(md5(i::text), 400) FROM generate_series(1, 300) i;
INSERT INTO narrow SELECT i, timestamp '2020-01-01' + i * interval '1 hour', repeat(md5(i::text), 4) FROM generate_series(1, 3000) i;
VACUUM (FREEZE) wide, narrow;
SELECT count(*) AS rows, sum(length(body)) = 300 * 12800 AS bytes_kept FROM wide;

SELECT sift.set_tier_policy('wide', 'file://' || :'tierdir', '30 days') AS wide_policy;
SELECT sift.set_tier_policy('narrow', 'file://' || :'tierdir', '30 days') AS narrow_policy;
VACUUM wide, narrow;
SELECT relation, count(*) AS tiered FROM sift.tier_status GROUP BY relation ORDER BY 1::text;
SELECT count(*) AS rows, sum(length(body)) = 300 * 12800 AS bytes_kept FROM wide;
SELECT count(*) AS problems FROM sift.check('wide');

SELECT sift.clear_tier_policy('wide') AS wide_cleared;
SELECT pg_relation_size('wide') > 0 AS has_blocks;
SELECT pg_relation_size('wide') AS before_ttl \gset
SELECT sift.set_ttl('wide', 'ts', interval '30 days') AS ttl;
VACUUM wide;
SELECT count(*) AS rows FROM wide;
SELECT count(*) AS problems FROM sift.check('wide');
INSERT INTO wide SELECT i, timestamp '2020-01-01' + i * interval '1 hour', repeat(md5(i::text), 400) FROM generate_series(1, 300) i;
VACUUM (FREEZE) wide;
SELECT pg_relation_size('wide') <= :before_ttl * 1.25 AS wide_extents_reused;
SELECT count(*) AS problems FROM sift.check('wide');

DROP TABLE wide, narrow;
\set rm 'cat >/dev/null && rm -rf ' :tierdir
COPY (SELECT 1) TO PROGRAM :'rm';
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
