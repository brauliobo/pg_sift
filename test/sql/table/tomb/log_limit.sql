-- Every backend that reads a sift_table keeps the tombstone log of the table sorted (64 bytes an entry with the scratch of a first look) up to what
-- sift.backend_cache_kb allows, and the entries past it in sorted runs on temp files: a read and a statement of any number of rows complete, with the memory of the backend
-- following the setting and not the log (test/t/table/tomb_spill.sh compares the rows with a heap twin). The test lowers the setting to 16 kB, 256 entries.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;

CREATE TABLE tl (id int, v int) USING sift_table;
INSERT INTO tl SELECT i, i % 7 FROM generate_series(1, 4000) i;
VACUUM tl;
DELETE FROM tl WHERE id % 4 = 0;
SELECT count(*) AS default_limit FROM tl;
-- a new backend reads the log from its start
\c -
SET sift.backend_cache_kb = '16kB';
SELECT count(*) AS lowered, sum(v) AS lowered_sum FROM tl WHERE id % 4 <> 0;
SELECT count(*) AS lowered_all FROM tl;
-- under a small work_mem the log spills while the caches are far from their limit: its runs on temp files last as long as the transaction that read it
\c -
SET work_mem = '64kB';
SELECT count(*) AS small_work_mem FROM tl WHERE id % 4 <> 0;
SELECT count(*) AS temp_files_left FROM pg_ls_tmpdir() WHERE name LIKE 'pgsql_tmp' || pg_backend_pid() || '.%';
-- VACUUM consumes the log, whatever its length: the table reads the same
VACUUM tl;
SELECT count(*) AS after_vacuum FROM tl;

-- the entries of a statement's own rows count as well: a DELETE of more rows than the limit holds completes
CREATE TABLE tm (id int) USING sift_table;
INSERT INTO tm SELECT i FROM generate_series(1, 2000) i;
\c -
SET sift.backend_cache_kb = '16kB';
DELETE FROM tm;
SELECT count(*) AS left_rows FROM tm;

DROP TABLE tm;
DROP TABLE tl;
DROP EXTENSION pg_sift;
