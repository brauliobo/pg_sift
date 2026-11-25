-- The relcache entry of a sift_col index is invalidated where a lock or a catalog lookup of the first touch of the index state (Format 2: the derived layout of the index, built when the
-- first scan, write or aggregate asks) and of the begin of a Format 1 scan (the heap is opened, the quals of a numeric or text column read the catalogs) reloads it, as a REINDEX CONCURRENTLY
-- of another session does: the cached state of the entry is overwritten and freed. What the first touch builds is attached to the state the entry holds after the lookups, and the
-- Format 1 scan keeps its own copy of what it read, so every answer is the heap twin's and no backend crashes.
-- requires: build col
\set ON_ERROR_STOP on
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

CREATE FUNCTION same(q text, heap text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE 'SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (' || q || ') x' INTO a;
	EXECUTE 'SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (' || regexp_replace(q, '\mpc\M', heap, 'g') || ') x' INTO b;
	RETURN CASE WHEN a = b THEN 'same' ELSE 'DIFFERENT' END;
END $$;

CREATE FUNCTION checks() RETURNS TABLE (shape text, answer text) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT 'numeric scan', same('SELECT id, n FROM pc WHERE n > 120 AND g = 3', 'ph');
	RETURN QUERY SELECT 'numeric list scan', same('SELECT id FROM pc WHERE n IN (1.25, 2.5, 20)', 'ph');
	RETURN QUERY SELECT 'text scan', same('SELECT id, tag FROM pc WHERE tag > ''tag2'' AND v < 9', 'ph');
	RETURN QUERY SELECT 'numeric aggregate', same('SELECT tag, sum(n), avg(n) FROM pc WHERE n > 20 GROUP BY tag', 'ph');
	RETURN QUERY SELECT 'text aggregate', same('SELECT g, count(*) FROM pc WHERE tag > ''tag2'' AND v < 9 GROUP BY g', 'ph');
END $$;

CREATE TABLE ph (id int, g int, v int, n numeric(12,2), tag text);
INSERT INTO ph SELECT i, i % 7, i % 13, (i % 997) / 8.0, 'tag' || i % 5 FROM generate_series(1, 20000) i;

-- Format 2: every invalidation frees the state before the derived layout of the index is attached to it
SET sift.test_create_format1 = off;
CREATE TABLE pc (LIKE ph);
INSERT INTO pc SELECT * FROM ph;
CREATE INDEX pc_col ON pc USING sift_col (g, v, n, tag);
ANALYZE ph; ANALYZE pc;
SELECT sift._test_index_format('pc_col') AS format_major;
SET sift.test_col_rel_inval = on;
SELECT * FROM checks();
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT * FROM checks();
RESET enable_bitmapscan;
RESET enable_seqscan;
RESET sift.test_col_rel_inval;
DROP TABLE pc;

-- Format 1: the begin of a bitmap scan and of an index scan opens the heap and reads the quals after it took what it keeps from the state
SET sift.test_create_format1 = on;
CREATE TABLE pc (LIKE ph);
INSERT INTO pc SELECT * FROM ph;
CREATE INDEX pc_col ON pc USING sift_col (g, v, n, tag);
ANALYZE pc;
SELECT sift._test_index_format('pc_col') AS format_major;
SET sift.test_col_rel_inval = on;
SET enable_seqscan = off;
SET enable_indexscan = off;
SELECT * FROM checks();
RESET enable_indexscan;
SET enable_bitmapscan = off;
SELECT * FROM checks();
RESET enable_bitmapscan;
RESET enable_seqscan;
RESET sift.test_col_rel_inval;
RESET sift.test_create_format1;
DROP TABLE ph, pc;
DROP FUNCTION checks(), same(text, text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
