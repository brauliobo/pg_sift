-- sift.index_size_breakdown and sift.status of a relation with parts in object storage: no category is larger than the relation, the pages of the column deltas of a sift_table are their
-- own category, and the status is the manifest's own: it does not open the objects, so it answers while the store is out of reach.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse
SELECT current_setting('data_directory') || '/sift_tier_regress_inspect' AS tierdir \gset
\set mk 'cat >/dev/null && rm -rf ' :tierdir ' && mkdir -p ' :tierdir
COPY (SELECT 1) TO PROGRAM :'mk';
CREATE TABLE evi (id int8, ts timestamp NOT NULL, v int, pad text);
INSERT INTO evi SELECT i, timestamp '2020-01-01' + i * interval '1 hour', i % 100, md5(i::text) FROM generate_series(1, 5000) i;
CREATE INDEX evi_col ON evi USING sift_col (id, ts, v, pad);
VACUUM evi;
INSERT INTO evi SELECT 5000 + i, localtimestamp - i * interval '1 minute', i % 100, md5((5000 + i)::text) FROM generate_series(1, 5000) i;
SELECT sift.set_tier_policy('evi_col', 'file://' || :'tierdir', '30 days') AS changed;
VACUUM evi;
SELECT count(*) > 0 AS tiered FROM sift.tier_status;

-- no category exceeds the relation, and the categories are exactly the file when every part is local
SELECT bool_and(bytes >= 0 AND bytes <= pg_relation_size('evi_col')) AS none_beyond_the_file, sum(bytes) = pg_relation_size('evi_col') AS adds_up FROM sift.index_size_breakdown('evi_col');
SELECT bool_and(bytes >= 0 AND bytes <= pg_relation_size('evi_col')) AS sections_within_the_file FROM sift.col_sections('evi_col');

-- the store goes away: the status still says what the manifest knows
\set rm 'cat >/dev/null && rm -rf ' :tierdir
COPY (SELECT 1) TO PROGRAM :'rm';
SELECT l0_parts + l1_parts + l2_parts > 0 AS has_parts, live_rows = 10000 AS rows_known FROM sift.status('evi_col');

-- the updates of a sift_table are column deltas: their pages are a category of their own and not manifest metadata
CREATE TABLE evt (id int8 PRIMARY KEY, v int, pad text) USING sift_table;
INSERT INTO evt SELECT i, i % 100, md5(i::text) FROM generate_series(1, 3000) i;
VACUUM evt;
UPDATE evt SET v = v + 1 WHERE id % 3 = 0;
SELECT count(*) FILTER (WHERE category = 'col_delta' AND bytes > 0) AS col_delta_category, sum(bytes) = pg_relation_size('evt') AS adds_up FROM sift.index_size_breakdown('evt');
SELECT count(*) AS problems FROM sift.check('evt');

DROP TABLE evt;
DROP TABLE evi;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
