-- The planner prices an index for the share of its units that hold each pre-aggregate section. Reading the sections of every unit costs a few buffers each, on the
-- first plan of every backend, so the units are counted up to sift.test_limits pre_census_units (256 by default) together, spread over the segments in proportion to
-- their units, and each count stands for the units of its segment. An index of fewer units is counted whole.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;

-- one row to a heap page (a value of 7000 bytes that cannot be compressed or moved out of line), 256 pages to a unit: 6 units for each segment
CREATE TABLE cs (id int, kind int, val bigint, pad bytea) WITH (autovacuum_enabled = off);
ALTER TABLE cs ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO cs SELECT g, g % 6, g * 7, decode(repeat(md5(g::text), 437), 'hex') FROM generate_series(1, 1536) g;
CREATE INDEX cs_idx ON cs USING sift_col (id, kind, val) WITH (write_buffer_limit = 4096, cube_minmax = false, superunit = 0);
-- the option is set, and a second segment is written with the min and max the first has not
ALTER INDEX cs_idx SET (cube_minmax = true);
INSERT INTO cs SELECT g, g % 6, g * 7, decode(repeat(md5(g::text), 437), 'hex') FROM generate_series(1537, 3072) g;
SELECT sift.col_promote('cs_idx') >= 0 AS promoted;
VACUUM (freeze, disable_page_skipping) cs;
SELECT count(*) AS segments FROM sift._test_col_formats('cs_idx');

-- every unit counted: 12 units, all with the histogram of the leaf, half with its min and max
SELECT units, dim_units, mm_units FROM sift._test_col_pre_stats('cs_idx') WHERE leaf = 1;

-- two units of each segment, in a backend that has not counted them yet (the count is kept for the backend): the counts stand for the units of the segment
DO $$ BEGIN EXECUTE format('ALTER DATABASE %I SET sift.test_limits = ''pre_census_units=2''', current_database()); END $$;
\connect
SELECT units, dim_units, mm_units FROM sift._test_col_pre_stats('cs_idx') WHERE leaf = 1;
DO $$ BEGIN EXECUTE format('ALTER DATABASE %I RESET sift.test_limits', current_database()); END $$;

DROP TABLE cs;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
