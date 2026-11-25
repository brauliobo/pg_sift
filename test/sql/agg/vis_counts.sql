-- The planner reads the visibility map of a table twice for its statistics (the all-visible pages, and the superblocks whose pages all are: the units that
-- pre-aggregates may answer) and once more for the stamp that tells whether the facts it kept are stale: one pass over the map counts both. The function
-- sift._test_vis_counts gives {all-visible pages, clean superblocks} by that pass, and the same two numbers page by page from visibilitymap_get_status.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;

-- wide rows: a few per page, so the table has hundreds of pages and several superblocks (256 pages), the last one partial
CREATE TABLE vc_t (id int PRIMARY KEY, pad text) WITH (autovacuum_enabled = false, fillfactor = 100);
ALTER TABLE vc_t ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO vc_t SELECT g, repeat('x', 1500) FROM generate_series(1, 3000) g;
CREATE INDEX vc_col ON vc_t USING sift_col (id);
RESET client_min_messages;

CREATE FUNCTION vc_check(label text, nblocks int8 DEFAULT -1) RETURNS TABLE (state text, same_as_pages bool, visible_pages int8, clean_superblocks int8) LANGUAGE sql AS $f$
	SELECT label, c[1] = c[3] AND c[2] = c[4], c[1], c[2] FROM (SELECT sift._test_vis_counts('vc_t', nblocks) AS c) s $f$;

-- no page is all-visible before a vacuum, all are after it
SELECT * FROM vc_check('new rows');
VACUUM vc_t;
SELECT * FROM vc_check('after vacuum');

-- a row of one page, of the first superblock and of the last one, are changed: those pages and the superblocks they are in are not
UPDATE vc_t SET pad = repeat('y', 1500) WHERE id = 1;
SELECT * FROM vc_check('first superblock dirty');
UPDATE vc_t SET pad = repeat('y', 1500) WHERE id = 3000;
SELECT * FROM vc_check('first and last superblock dirty');
VACUUM vc_t;
SELECT * FROM vc_check('after vacuum again');

-- pages scattered over the superblocks
UPDATE vc_t SET pad = repeat('z', 1500) WHERE id % 211 = 0;
SELECT * FROM vc_check('scattered');

-- a table of a size that ends inside a map byte (pages 2 of 4), vacuumed and not
DO $$ BEGIN WHILE pg_relation_size('vc_t') / 8192 % 4 <> 2 LOOP INSERT INTO vc_t SELECT 10000 + g + (SELECT count(*) FROM vc_t), repeat('x', 1500) FROM generate_series(1, 5) g; END LOOP; END $$;
SELECT pg_relation_size('vc_t') / 8192 % 4 AS pages_mod_4;
-- the pages that read marked all-visible on their own (PostgreSQL 19 marks a few more than 17 and 18) are not the point: the two counts agree
SELECT state, same_as_pages, visible_pages BETWEEN 570 AND 600 AS visible_about, clean_superblocks FROM vc_check('size ends inside a map byte');
VACUUM vc_t;
SELECT * FROM vc_check('and vacuumed');

-- the pages counted are the first ones, whatever their number: a prefix that ends on a map byte, a word, a superblock, and none
SELECT n, (vc_check('prefix', n)).same_as_pages FROM unnest(ARRAY[0, 4, 32, 36, 252, 256, 260, 300, 512, 516, 1000]) n ORDER BY n;

-- the pure C count of the bits (sift.simd = scalar) and the instruction of the CPU (the default) count the same pages
SET sift.simd = 'scalar';
SELECT * FROM vc_check('scalar kernel');
SELECT n, (vc_check('scalar prefix', n)).same_as_pages FROM unnest(ARRAY[0, 4, 36, 256, 516, 1000]) n ORDER BY n;
RESET sift.simd;
SELECT * FROM vc_check('default kernel');

DROP FUNCTION vc_check;
DROP TABLE vc_t;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
