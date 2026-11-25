CREATE EXTENSION pg_sift_test CASCADE;
SET maintenance_work_mem = '1MB';

-- A spool reads back what was written, within its share of maintenance_work_mem (32 kB at 1 MB) and beyond it
SELECT n, sift._test_spool(n) AS same FROM (VALUES (0), (1), (4095), (4096), (32768), (32769), (1000000)) AS t (n);

-- Within the share it takes no temporary file; beyond it, it does
SET temp_file_limit = '64kB';
SELECT sift._test_spool(32768) AS within;
SELECT sift._test_spool(1000000) AS beyond;

DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
