-- A DELETE that kills more rows of one granule than a chunk of a deletion vector holds (2048) is written as several chunks, and the table reads back and vacuums without a damaged vector.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET client_min_messages = warning;

CREATE TABLE dc (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE dh (LIKE dc);
INSERT INTO dc SELECT g, repeat(md5(g::text), 4) FROM generate_series(1, 60000) g;
INSERT INTO dh SELECT * FROM dc;
DELETE FROM dc WHERE id > 6000;
DELETE FROM dh WHERE id > 6000;
SELECT count(*) AS rows_left, (SELECT count(*) FROM dh) AS heap_rows_left FROM dc;
VACUUM dc;
SELECT count(*) AS rows_after_vacuum, sum(id) = (SELECT sum(id) FROM dh) AS sum_matches_heap FROM dc;
SELECT count(*) AS check_problems FROM sift.check('dc'::regclass);

DROP TABLE dc, dh;
DROP EXTENSION pg_sift;
