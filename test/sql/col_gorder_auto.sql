-- order_array_columns is chosen by the build when it is unset or 'auto': a column of 100,000 rows or more with 1,000 or more distinct values whose values are
-- scattered over the heap (correlation under 0.99) gets the global order, so that a range of it reads the entries between its bounds; a column in heap order, with few values, on
-- a smaller table or a narrow index gets none; a table without statistics is analyzed first. A date or timestamp comes before the other columns, as a range or an
-- ORDER BY .. LIMIT is what it is read by. 'off' and a list are the user's.
-- The pad column has strings over 64 bytes: a text column of shorter strings is chosen too (col_gorder_text), and the array holds the strings again, so these tests keep it out.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE ga (id int, r int, g int, ts timestamp, pad text);
ALTER TABLE ga ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO ga SELECT i, abs(hashint4(i)) % 5000000, i % 50, timestamp '2024-01-01' + i * interval '1 second', md5(i::text) || md5(i::text || 'x') FROM generate_series(1, 120000) i;

-- no statistics yet: the build analyzes the table and chooses
CREATE INDEX ga_nostats ON ga USING sift_col (id, r, g, ts, pad);
SELECT reloptions FROM pg_class WHERE relname = 'ga_nostats';
DROP INDEX ga_nostats;

ANALYZE ga;
-- r is scattered, g has 50 values, id and ts follow the heap
CREATE INDEX ga_auto ON ga USING sift_col (id, r, g, ts, pad);
SELECT reloptions FROM pg_class WHERE relname = 'ga_auto';
SELECT min(format) AS format FROM sift.col_segments('ga_auto') WHERE kind = 'immutable';
-- a range of r reads the addresses and no unit
SELECT p.units AS units_with_addresses, p.rows = 0 AS no_row_read, f.units > p.units AS fewer_units
FROM sift._test_col_counters('ga_auto', '1:ge:2000000;1:le:2004000', 24) p, sift._test_col_counters('ga_auto', '1:ge:2000000;1:le:2004000', 0) f;

-- off and a list are the user's
CREATE INDEX ga_off ON ga USING sift_col (id, r, g, ts, pad) WITH (order_array_columns = off);
CREATE INDEX ga_list ON ga USING sift_col (id, r, g, ts, pad) WITH (order_columns = 'g', order_array_columns = 'g');
SELECT relname, reloptions FROM pg_class WHERE relname IN ('ga_off', 'ga_list') ORDER BY 1;
-- the update list is for an option that names columns: off is not a gain to reindex for
SELECT count(*) AS commands FROM sift.upgrade_commands() WHERE object = 'ga_off'::regclass;
DROP INDEX ga_auto, ga_off, ga_list;

-- a narrow index of small columns: the arrays would be a third of it
CREATE INDEX ga_narrow ON ga USING sift_col (id, r, g) WITH (point_tid_columns = 'off');
SELECT reloptions FROM pg_class WHERE relname = 'ga_narrow';
DROP INDEX ga_narrow;

-- a table under 100,000 rows
CREATE TABLE gs AS SELECT id, r, g, ts, pad FROM ga WHERE id <= 90000;
ANALYZE gs;
CREATE INDEX gs_i ON gs USING sift_col (id, r, g, ts, pad);
SELECT reloptions FROM pg_class WHERE relname = 'gs_i';

-- the choice is made again by REINDEX: after the statistics change the column is dropped
CREATE INDEX ga_re ON ga USING sift_col (id, r, g, ts, pad);
SELECT reloptions FROM pg_class WHERE relname = 'ga_re';
UPDATE ga SET r = id;
ANALYZE ga;
REINDEX INDEX ga_re;
SELECT reloptions FROM pg_class WHERE relname = 'ga_re';

-- a table never analyzed, a timestamp at random in the heap and another column of more values: both are chosen, the timestamp first
CREATE TABLE gt (id int, v int, ts timestamptz, pad text);
ALTER TABLE gt ALTER COLUMN pad SET STORAGE PLAIN;
SELECT setseed(0.5);
INSERT INTO gt SELECT i, (random() * 100000000)::int, timestamptz '2024-01-01' + random() * interval '700 days', md5(i::text) || md5(i::text || 'x') FROM generate_series(1, 120000) i;
CREATE INDEX gt_i ON gt USING sift_col (id, v, ts, pad);
SELECT reloptions FROM pg_class WHERE relname = 'gt_i';

-- a REINDEX cannot analyze (ANALYZE opens the index it rebuilds): a table without statistics chooses nothing
DELETE FROM pg_statistic WHERE starelid = 'gt'::regclass;
ALTER INDEX gt_i RESET (order_array_columns);
REINDEX INDEX gt_i;
REINDEX TABLE gt;
SELECT reloptions FROM pg_class WHERE relname = 'gt_i';

DROP TABLE ga, gs, gt;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
