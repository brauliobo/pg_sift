-- A build that fills its memory many times writes one part of the cold tier for each run and puts them in the manifest a page of parts at a time: every run is there once, at the level of
-- a compaction, the rows add up, the keys are in order and the reads agree with the heap.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE FUNCTION f2_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE via_index text; via_heap text;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = on;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_index;
	SET LOCAL enable_seqscan = on; SET LOCAL enable_bitmapscan = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_heap;
	RETURN CASE WHEN via_index = via_heap THEN 'same' ELSE 'DIFFERENT: ' || via_index || ' / ' || via_heap END;
END $$;

CREATE TABLE runs (id bigint, grp int, tag text);
INSERT INTO runs SELECT i, i % 7, CASE WHEN i % 11 = 0 THEN NULL ELSE 'tag-' || (i % 500) END FROM generate_series(1, 2000000) i;
SET maintenance_work_mem = '1MB';
CREATE INDEX runs_idx ON runs USING sift_col (id, grp, tag) WITH (key_order = 'id');
RESET maintenance_work_mem;

SELECT count(*) > 36 AS more_runs_than_one_page, count(DISTINCT part_gen) = count(*) AS one_gen_each, min(level) = 2 AND max(level) = 2 AS level_of_a_compaction,
	   sum(nrows) = 2000000 AS rows_add_up, sum(ndead) = 0 AS none_dead
FROM sift._test_f2_parts('runs_idx', 1, NULL);
SELECT regexp_replace(sift._test_f2_check_keys('runs_idx'), 'parts=\d+ granules=\d+', 'parts=N granules=N') AS keys;
SELECT f2_same('SELECT id, grp, tag FROM runs WHERE id BETWEEN 1999000 AND 1999500') AS range_read,
	   f2_same('SELECT grp, count(*), count(tag), count(DISTINCT tag) FROM runs GROUP BY grp') AS aggregate_read,
	   f2_same('SELECT id FROM runs WHERE tag = ''tag-7'' AND id % 1000 = 7') AS filter_read;

DROP TABLE runs;
DROP FUNCTION f2_same(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
