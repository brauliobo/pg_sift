-- sift.col_index_status: the sizes, the dead rows, the write buffer and the reason a rewrite is due, from a fresh index through a DELETE and
-- VACUUM to the rewrite, and only for the indexes whose table the caller can read. The statistics of the table (last VACUUM, rows changed) lag
-- the statements and are the test of sift.col_maintenance_status's.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
CREATE TABLE cis (id int, k int, v float8, pad text) WITH (autovacuum_enabled = off);
INSERT INTO cis SELECT g, g % 7, g, md5(g::text) FROM generate_series(1, 20000) g;
CREATE INDEX cis_idx ON cis USING sift_col (id, k, v, pad);

-- fresh: one segment, nothing dead, nothing buffered, nothing due
SELECT "table", segments, max_segments, segment_rows, units = listed_units AS units_once, dirty_segments, dead_rows, buffer_rows,
       sealed_buffers, running, failures, due, index_bytes >= segment_bytes AS sized
FROM sift.col_index_status WHERE index = 'cis_idx'::regclass;

-- a write buffer: new rows wait there until a promotion
INSERT INTO cis SELECT g, g % 7, g, md5(g::text) FROM generate_series(20001, 20100) g;
SELECT segments, buffer_rows FROM sift.col_index_status WHERE index = 'cis_idx'::regclass;

-- one row in twenty dies, so every unit holds a dead row: VACUUM marks the units dirty and the rewrite is due for them (the rows are wide: their scans cost
-- less than the rewrite of the segment, which VACUUM leaves for sift.col_maintain)
DELETE FROM cis WHERE id % 20 = 0;
VACUUM cis;
SELECT dirty_segments > 0 AS dirty, dead_rows > 0 AS dead, dead_pct, due FROM sift.col_index_status WHERE index = 'cis_idx'::regclass;

-- the rewrite writes a clean segment; the second entry is the empty write buffer that is open again
SELECT sift.col_maintain('cis_idx');
SELECT segments, dirty_segments, dead_rows, buffer_rows, due FROM sift.col_index_status WHERE index = 'cis_idx'::regclass;

-- a role that cannot read the table does not see the index
CREATE ROLE cis_non;
SET ROLE cis_non;
SELECT count(*) FROM sift.col_index_status;
RESET ROLE;
DROP ROLE cis_non;

DROP TABLE cis;
DROP EXTENSION pg_sift;
