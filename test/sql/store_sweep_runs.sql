-- The orphan sweep releases the blocks no directory entry refers to as extents, one for each run of consecutive blocks: the array it builds follows
-- the runs, not the blocks, so that the orphans of a killed write of many gigabytes (whole extents) need a few bytes. sift.test_limits lowers
-- array_bytes, the limit of the helper the array is allocated with, below what one extent a block would take.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE TABLE sw (v int);
CREATE INDEX sw_idx ON sw USING sift_store_test (v);

-- 37 extents of 32 blocks allocated one after the other and never released: 1184 consecutive orphan blocks
SELECT sift._test_alloc('sw_idx', 32, 37, 0) > 0 AS allocated;
SELECT sift._test_check('sw_idx') AS orphans;
SET sift.test_limits = 'array_bytes=1024';
SELECT sift._test_sweep('sw_idx') AS swept;
RESET sift.test_limits;
SELECT sift._test_check('sw_idx') AS orphans;

-- orphans that are not consecutive take an extent each: every second extent released, the others swept (600 runs, 4.8 kB) against a limit under it
SELECT sift._test_alloc('sw_idx', 1, 1200, 2) > 0 AS fragmented;
SELECT sift._test_check('sw_idx') AS orphans;
SET sift.test_limits = 'array_bytes=1024';
CREATE FUNCTION sw_fails() RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift._test_sweep('sw_idx');
	RETURN false;
EXCEPTION WHEN program_limit_exceeded THEN
	RETURN true;
END $$;
SELECT sw_fails() AS fails_over_the_limit;
RESET sift.test_limits;
SELECT sift._test_sweep('sw_idx') > 0 AS swept;
SELECT sift._test_check('sw_idx') AS orphans;

DROP FUNCTION sw_fails();
DROP TABLE sw;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
