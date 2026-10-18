-- Rows above half a page keep their overflow pages in the delta segment of their record, also when the segment fills up in the middle
-- of a row (the bytes used to land in the next segment and the read failed with a generation error).
CREATE EXTENSION pg_sift;
-- the rows are read from the table; an aggregate scan refuses a column group that cannot hold such values
SET sift.enable_agg = off;

CREATE TABLE wide (id int, v text) USING sift_table;
-- 4100 .. 34100 bytes per row: 2 to 5 pages each, so that the 512 pages of the first delta segment end inside some row
INSERT INTO wide SELECT g, repeat(chr(97 + g % 26), 4100 + (g % 7) * 5000) FROM generate_series(1, 260) g;
SELECT count(*), sum(length(v)) FROM wide;
SELECT count(*) AS wrong FROM wide WHERE v <> repeat(chr(97 + id % 26), 4100 + (id % 7) * 5000);
SELECT count(DISTINCT (ctid::text::point)[0]::int / 512) > 1 AS several_segments FROM wide;

-- the same through updates, and after VACUUM
UPDATE wide SET v = repeat('u', 9000 + (id % 3) * 9000) WHERE id % 2 = 0;
SELECT count(*) AS wrong FROM wide WHERE id % 2 = 0 AND v <> repeat('u', 9000 + (id % 3) * 9000);
VACUUM wide;
SELECT count(*), sum(length(v)) FROM wide;

-- a row that cannot fit a delta segment is refused, not corrupted
INSERT INTO wide VALUES (0, repeat('x', 5000000));
SELECT count(*) FROM wide;
DROP TABLE wide;
DROP EXTENSION pg_sift;
