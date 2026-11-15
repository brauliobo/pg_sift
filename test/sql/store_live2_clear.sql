-- Killing a doc of a segment of format 2 sets the dirty bit of its superblock inside the LIVE run, whatever page of the run the bit lies on,
-- and counts it dead in the header: a run of more than 65151 superblocks keeps its dirty bitmap over several pages
CREATE EXTENSION pg_sift_test CASCADE;
CREATE TABLE t (v int);
CREATE INDEX t_idx ON t USING sift_store_test (v);

SELECT nsb, sbidx, damaged, dirty, ndead, dead_dl
FROM (VALUES (64, 0), (64, 63), (70000, 0), (70000, 65151), (70000, 65152), (70000, 65216), (70000, 65261),
			 (70000, 65535), (70000, 69999), (600000, 599999)) AS c (nsb, sbidx),
	 LATERAL sift._test_live2_clear('t_idx', nsb, sbidx);

DROP TABLE t;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
