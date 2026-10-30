-- Rows above half a page keep their overflow pages in the write buffer of their record, also when the segment fills up in the middle
-- of a row (the bytes used to land in the next segment and the read failed with a generation error).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;

CREATE TABLE wide (id int, v text) USING sift_table;
-- 4100 .. 34100 bytes per row: 2 to 5 pages each, so that the 512 pages of the first write buffer end inside some row
INSERT INTO wide SELECT g, repeat(chr(97 + g % 26), 4100 + (g % 7) * 5000) FROM generate_series(1, 260) g;
SELECT count(*), sum(length(v)) FROM wide;
SELECT count(*) AS wrong FROM wide WHERE v <> repeat(chr(97 + id % 26), 4100 + (id % 7) * 5000);
SELECT count(DISTINCT (ctid::text::point)[0]::int / 512) > 1 AS several_segments FROM wide;

-- the same through updates, and after VACUUM
UPDATE wide SET v = repeat('u', 9000 + (id % 3) * 9000) WHERE id % 2 = 0;
SELECT count(*) AS wrong FROM wide WHERE id % 2 = 0 AND v <> repeat('u', 9000 + (id % 3) * 9000);
VACUUM wide;
SELECT count(*), sum(length(v)) FROM wide;

-- SiftAgg reads text of any length from the promoted units (and the write buffer rows): the same rows as the plain plan
CREATE TABLE longtxt (id int, g int, v text) USING sift_table;
INSERT INTO longtxt SELECT i, i % 7, (SELECT string_agg(md5((i % 50) || ':' || k), '') FROM generate_series(1, 10 + i % 50) k) FROM generate_series(1, 3000) i;
CREATE FUNCTION long_rows(q text, push bool) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.rows_agg(q, push) $$;
CREATE FUNCTION long_pushed(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_mentions_siftagg(q) $$;
CREATE TABLE long_q (q text);
INSERT INTO long_q VALUES
	('SELECT count(DISTINCT v), count(*) FROM longtxt'),
	('SELECT v, count(*) FROM longtxt GROUP BY v'),
	('SELECT count(*) FROM longtxt WHERE v = repeat(''d'', 307)'),
	('SELECT count(*) FROM longtxt WHERE v LIKE ''c4ca%'' COLLATE "C"'),
	('SELECT g, count(*), count(DISTINCT v) FROM longtxt WHERE id > 100 GROUP BY g');
SELECT q, long_pushed(q) AS pushed, long_rows(q, true) IS NOT DISTINCT FROM long_rows(q, false) AS same FROM long_q ORDER BY q COLLATE "C";
VACUUM longtxt;
SELECT q, long_pushed(q) AS pushed, long_rows(q, true) IS NOT DISTINCT FROM long_rows(q, false) AS same FROM long_q ORDER BY q COLLATE "C";
INSERT INTO longtxt SELECT 5000 + i, i % 7, repeat('d', 300 + i) FROM generate_series(1, 20) i;
UPDATE longtxt SET v = v || 'x' WHERE id % 11 = 0;
DELETE FROM longtxt WHERE id % 5 = 0;
SELECT q, long_pushed(q) AS pushed, long_rows(q, true) IS NOT DISTINCT FROM long_rows(q, false) AS same FROM long_q ORDER BY q COLLATE "C";
VACUUM longtxt;
SELECT q, long_pushed(q) AS pushed, long_rows(q, true) IS NOT DISTINCT FROM long_rows(q, false) AS same FROM long_q ORDER BY q COLLATE "C";
DROP TABLE longtxt, long_q;
DROP FUNCTION long_rows, long_pushed;

-- a row that cannot fit a write buffer is refused, not corrupted
SET sift.table_wide_rows = off;
INSERT INTO wide VALUES (0, repeat('x', 5000000));
RESET sift.table_wide_rows;
SELECT count(*) FROM wide;
DROP TABLE wide;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
