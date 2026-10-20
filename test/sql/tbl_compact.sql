-- Compaction at VACUUM writes the segment that ends the file again below, into the free blocks of the write buffers it replaced, in the
-- same VACUUM (the relation is locked exclusively for that, so what the VACUUM released is reusable at once). A segment
-- written for the column layout before an ALTER TABLE cannot be written again by the new layout: it stays where it is and VACUUM goes on.
CREATE EXTENSION pg_sift;
CREATE TABLE cp (id int, v bigint, t text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TEMP TABLE sz (round int, blocks bigint);
\set fill 'INSERT INTO cp SELECT g, g * 3, ''text '' || md5(g::text) FROM generate_series(1, 30000) g'

-- the segment sealed from the load lies above the write buffer it replaced; with its layout current the VACUUM moves it down
:fill;
INSERT INTO sz SELECT 1, relation_blocks FROM sift.table_space('cp');
VACUUM cp;
SELECT relation_blocks < (SELECT blocks FROM sz WHERE round = 1) AS moved_down FROM sift.table_space('cp');
SELECT count(*), sum(v) FROM cp;

-- the same after the layout changed: the old segment is not written again, nothing fails, and the rows are all there
:fill;
VACUUM cp;
ALTER TABLE cp ADD COLUMN w int DEFAULT 7;
SELECT pg_current_xact_id() IS NOT NULL AS xid_passed;
VACUUM cp;
SELECT pg_current_xact_id() IS NOT NULL AS xid_passed;
VACUUM cp;
SELECT count(*), sum(v), sum(w) FROM cp;
VACUUM FULL cp;
SELECT count(*), sum(v), sum(w) FROM cp;
DROP TABLE cp;
DROP EXTENSION pg_sift;
