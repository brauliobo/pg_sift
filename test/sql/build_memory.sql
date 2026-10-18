-- CREATE INDEX at the smallest memory takes the same memory whatever the table size, a term in nine of ten
-- rows included: the runs a hot term makes stop growing when a segment's memory holds them, so the
-- segment writer never takes a term of the whole table. sift.test_build_peak is the most memory (kB) the
-- build allocated; the test limits are 500 documents a run, merges of 8 runs and 20,000 postings of the
-- largest term of a segment (the real one is a tenth of the memory budget).
CREATE EXTENSION pg_sift;
SET max_parallel_maintenance_workers = 0;
SET maintenance_work_mem = '1MB';
SET sift.test_run_docs = 500;
SET sift.test_merge_fanin = 8;
SET sift.test_segment_postings = 20000;

CREATE TABLE bm (id serial, body text);
INSERT INTO bm (body)
SELECT CASE WHEN i % 10 < 9 THEN 'hot ' ELSE '' END || 'u' || i || ' g' || (i % 1000) || ' a' || (i % 37)
FROM generate_series(1, 400000) i;

CREATE TABLE peaks (rows int, bytes bigint);
SET sift.test_build_peak = 0;
CREATE INDEX bm_1x ON bm USING sift (body) WHERE id <= 25000;
INSERT INTO peaks SELECT 25000, pg_size_bytes(current_setting('sift.test_build_peak'));
SET sift.test_build_peak = 0;
CREATE INDEX bm_4x ON bm USING sift (body) WHERE id <= 100000;
INSERT INTO peaks SELECT 100000, pg_size_bytes(current_setting('sift.test_build_peak'));
SET sift.test_build_peak = 0;
CREATE INDEX bm_16x ON bm USING sift (body);
INSERT INTO peaks SELECT 400000, pg_size_bytes(current_setting('sift.test_build_peak'));

-- the participants of a parallel build (one leader and two workers, each with a share of the budget)
SET max_parallel_maintenance_workers = 2;
ALTER TABLE bm SET (parallel_workers = 2);
SET sift.test_build_peak = 0;
CREATE INDEX bm_par_1x ON bm USING sift (body) WHERE id <= 25000;
INSERT INTO peaks SELECT 25001, pg_size_bytes(current_setting('sift.test_build_peak'));
SET sift.test_build_peak = 0;
CREATE INDEX bm_par_16x ON bm USING sift (body) WHERE id > 1;
INSERT INTO peaks SELECT 400001, pg_size_bytes(current_setting('sift.test_build_peak'));

-- the peak at 16 times the rows is that of 1 times (within half a MB) and a few times the 1 MB budget
SELECT max(bytes) FILTER (WHERE rows = 400000) <= max(bytes) FILTER (WHERE rows = 25000) + 524288 AS flat_16x,
       max(bytes) FILTER (WHERE rows = 100000) <= max(bytes) FILTER (WHERE rows = 25000) + 524288 AS flat_4x,
       max(bytes) FILTER (WHERE rows = 400001) <= max(bytes) FILTER (WHERE rows = 25001) + 524288 AS flat_parallel,
       max(bytes) <= 4 * 1048576 AS bounded
FROM peaks;

-- the segments take the table: the hot term is in segments of the limit, within the directory
SELECT count(*) BETWEEN 2 AND 128 AS segments_1x FROM sift.segments('bm_1x');
SELECT count(*) BETWEEN 10 AND 128 AS segments_16x FROM sift.segments('bm_16x');
SELECT sift.verify('bm_16x', true);

SET sift.enable_scan = off;
SELECT count(*) AS hot_seq FROM bm WHERE body @@> 'hot';
RESET sift.enable_scan;
SELECT count(*) AS hot_index FROM bm WHERE body @@> 'hot';

DROP TABLE bm, peaks;
DROP EXTENSION pg_sift;
