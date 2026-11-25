-- A VACUUM someone runs of a Format 1 sift_table (the 5.x layout) merges every dirty segment in one pass, whatever sift.vacuum_rewrite_limit says: the limit holds back an autovacuum pass
-- only, and the passes of VACUUM FREEZE and of a VACUUM of a person are never cut short. 40,000 rows a batch: one segment of about 0.9 MB and one of 0.2 MB.
CREATE EXTENSION pg_sift;
SET sift.test_create_format1 = on;
SET sift.async_maintenance = off;
SET client_min_messages = warning;
CREATE TABLE cap (id int, pad text) USING sift_table;
SELECT sift.table_set_option('cap', 'auto_defrag', 'off');
INSERT INTO cap SELECT i, md5(i::text) || md5((i + 1)::text) FROM generate_series(1, 40000) i;
SELECT busy FROM sift.table_maintain('cap');
INSERT INTO cap SELECT i, md5(i::text) || md5((i + 1)::text) FROM generate_series(40001, 80000) i;
SELECT busy FROM sift.table_maintain('cap');
INSERT INTO cap SELECT i, md5(i::text) || md5((i + 1)::text) FROM generate_series(80001, 120000) i;
SELECT busy FROM sift.table_maintain('cap');
CREATE TEMP TABLE before AS SELECT gen FROM sift.table_segments('cap') WHERE kind = 'columns';
SELECT count(*) >= 4 AS segments, sum(bytes) > 3 * 1024 * 1024 AS over_three_limits FROM sift.table_segments('cap') WHERE kind = 'columns';
DELETE FROM cap WHERE id % 2 = 0;
SET sift.vacuum_rewrite_limit = 1;
VACUUM cap;
SELECT count(*) FILTER (WHERE gen NOT IN (SELECT gen FROM before)) > 0 AS rewrote_some,
       count(*) FILTER (WHERE gen IN (SELECT gen FROM before)) = 0 AS rewrote_all_in_one_pass
  FROM sift.table_segments('cap') WHERE kind = 'columns';
SELECT count(*), sum(id) FROM cap;
DROP TABLE cap;
DROP EXTENSION pg_sift;
