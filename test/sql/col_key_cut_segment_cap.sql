-- A key_cut build of a table that makes more runs than it may open at once merges them as it scans, and the segments it ends with are the partitions of its writer: each is cut at the
-- budget of the writer (here 64 kB, 23,000 to 48,000 rows of this table) and no count of segments, the directory's or any other, shapes them. The arrays its end builds are up to 48 bytes
-- a row, so the memory of its end is bounded by the budget of a partition, never by the key or the table, and the participants of a parallel build leave the merge to slices of the same size.
-- sift.test_limits makes the arrays of the build fail over the limit, so the build itself proves the bound; a limit under it fails the build cleanly.
-- A key with a value that holds a third of the table is cut the same way: the rows of that value are read back in runs of the memory of one, not as one.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;

-- 3.2 million rows of 5,000 keys; the budget of a run (64 kB) is 20,000 rows or less: 160 runs
CREATE TABLE cap (id bigint, tenant_id int, d date) WITH (autovacuum_enabled = false, parallel_workers = 3);
INSERT INTO cap SELECT g, (g::bigint * 7919 % 5000)::int, date '2020-01-01' + (g % 2000) FROM generate_series(1, 3200000) g;
VACUUM (FREEZE, ANALYZE) cap;
SET sift.test_col_segment_kb = 64;

-- a partition of 50,000 rows is the most the arrays may hold: 2,400,000 bytes at 48 bytes a row
SET sift.test_limits = 'array_bytes=2400000';
CREATE INDEX cap_i ON cap USING sift_col (id, tenant_id, d) WITH (key_cut = 'tenant_id');
SELECT count(*) > 1 AS in_partitions, sum(rows) = 3200000 AS all_rows, max(rows) <= 50000 AS longest_within_a_partition
FROM sift.col_segments('cap_i') WHERE kind = 'immutable';
SELECT sift.col_verify('cap_i', true);
DROP INDEX cap_i;

-- a limit of half that (25,000 rows) fails the build with an error, not with a bad array
SET sift.test_limits = 'array_bytes=1200000';
CREATE FUNCTION cap_build_fails() RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
	CREATE INDEX cap_i ON cap USING sift_col (id, tenant_id, d) WITH (key_cut = 'tenant_id');
	RETURN false;
EXCEPTION WHEN program_limit_exceeded THEN
	RETURN true;
END $$;
SELECT cap_build_fails() AS fails_under_a_partition;
DROP FUNCTION cap_build_fails();
RESET sift.test_limits;

-- participants: the slices of the merge are of one size
SET max_parallel_maintenance_workers = 3;
SET min_parallel_table_scan_size = 0;
SET maintenance_work_mem = 65536;
CREATE INDEX cap_i ON cap USING sift_col (id, tenant_id, d) WITH (key_cut = 'tenant_id');
SELECT count(*) > 1 AS in_partitions, sum(rows) = 3200000 AS all_rows, max(rows) <= 1.25 * sum(rows) / count(*) AS slices_even
FROM sift.col_segments('cap_i') WHERE kind = 'immutable';
SELECT sift.col_verify('cap_i', true);
-- the merges after the build end when one writes the segments it read: 68 segments of the participants and a few merges, not a generation for every pass up to the limit
SELECT max(gen) < 100 AS merges_end FROM sift.col_segments('cap_i');

-- a third of the rows are one value of the key (the key_cut of the user's): the segments are those of an even key, in the same arrays, and the rows are the heap's (a segment is cut at the memory of its writer, so the last one holds what is left: up to 25% over the mean)
DROP TABLE cap;
SET max_parallel_maintenance_workers = 0;
CREATE TABLE cap (id bigint, tenant_id int, d date) WITH (autovacuum_enabled = false);
INSERT INTO cap SELECT g, CASE WHEN g % 3 = 0 THEN 1 ELSE (g::bigint * 7919 % 5000)::int END, date '2020-01-01' + (g % 2000) FROM generate_series(1, 3200000) g;
VACUUM (FREEZE, ANALYZE) cap;
SET sift.test_limits = 'array_bytes=2400000';
CREATE INDEX cap_i ON cap USING sift_col (id, tenant_id, d) WITH (key_cut = 'tenant_id');
SELECT count(*) > 1 AS in_partitions, sum(rows) = 3200000 AS all_rows, max(rows) <= 50000 AS longest_within_a_partition
FROM sift.col_segments('cap_i') WHERE kind = 'immutable';
SELECT sift.col_verify('cap_i', true);
RESET sift.test_limits;
SET sift.enable_agg = on;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SELECT count(*), sum(id) FROM cap WHERE tenant_id = 1;
SET sift.enable_agg = off;
SET enable_seqscan = on;
SELECT count(*), sum(id) FROM cap WHERE tenant_id = 1;

DROP TABLE cap;
DROP EXTENSION pg_sift;
