-- The LIVE run of a segment of more than 65151 superblocks that sift_live_clear of 4.2.1 to 4.3.5 damaged (the dirty bit of a superblock written on the first
-- page whatever its number, so in the special space of that page from superblock 65152 on): the audit finds what changed, the repair restores the special
-- space, sets the dirty bits that were lost and counts the dead rows by their bits, and a second repair writes nothing.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE TABLE t (v int);
CREATE INDEX t_idx ON t USING sift_store_test (v);

SELECT what, stage, damaged_pages, lost_units, ndead, cleared, written, dirty_read
FROM (VALUES ('first page', 70000, ARRAY[0, 65151]),
			 ('type, a bit already set', 70000, ARRAY[65152]),
			 ('type', 70000, ARRAY[65160]),
			 ('flags', 70000, ARRAY[65170]),
			 ('reserved', 70000, ARRAY[65200]),
			 ('generation', 70000, ARRAY[65261]),
			 ('header of the next page, lsn', 70000, ARRAY[65290]),
			 ('header of the next page, upper', 70000, ARRAY[65405]),
			 ('header of the next page, special', 70000, ARRAY[65410, 65440]),
			 ('counted twice', 70000, ARRAY[65170, 65170, 65261]),
			 ('every field', 70000, ARRAY[0, 65151, 65152, 65160, 65170, 65200, 65216, 65261, 65279]),
			 ('a run of 600,000', 600000, ARRAY[65261, 65270, 300000])) AS c (what, nsb, sbs),
	 LATERAL sift._test_live_repair('t_idx', nsb, sbs)
ORDER BY c.nsb, c.what, array_position(ARRAY['damaged', 'repaired', 'audit', 'again'], stage);

DROP TABLE t;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
