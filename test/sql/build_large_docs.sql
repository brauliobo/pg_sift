-- CREATE INDEX on a table of large documents in one heap page: the text of a page was copied into one string
-- of 1 GB at most, so three documents of 360 MB (out of line, compressed in the heap) failed the build.
-- Each document is analyzed on its own.
CREATE EXTENSION pg_sift;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE ld (id int, body text);
INSERT INTO ld SELECT i, repeat('internationalization localization ' || 'w' || i || ' ', 10000000) FROM generate_series(1, 3) i;
SELECT sum(length(body)) > 1073741823 AS over_a_gigabyte, count(DISTINCT (ctid::text::point)[0]) AS pages FROM ld;

CREATE INDEX ld_idx ON ld USING sift (body);
SELECT sift.verify('ld_idx', true);
SET sift.enable_scan = off;
SELECT id FROM ld WHERE body @@> 'w2' ORDER BY id;
RESET sift.enable_scan;
SELECT id FROM ld WHERE body @@> 'w2' ORDER BY id;
SELECT count(*) AS docs FROM ld WHERE body @@> 'internationalization localization';

DROP TABLE ld;
DROP EXTENSION pg_sift;
