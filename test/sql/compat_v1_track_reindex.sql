-- A Format 1 sift_col index with track_changes (ctid and a key) moved to Format 2.0 by REINDEX INDEX CONCURRENTLY: its Format 1 triggers stay with the index, and UPDATE, DELETE and
-- the reads that follow answer as the heap.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE via_index text; via_heap text;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = on;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_index;
	SET LOCAL enable_seqscan = on; SET LOCAL enable_bitmapscan = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_heap;
	RETURN CASE WHEN via_index = via_heap THEN 'same' ELSE 'DIFFERENT' END;
END $$;

SET sift.test_create_format1 = on;
CREATE TABLE tc (id int PRIMARY KEY, g int, v int);
CREATE TABLE tk (id int PRIMARY KEY, g int, v int);
INSERT INTO tc SELECT i, i % 11, i % 97 FROM generate_series(1, 5000) i;
INSERT INTO tk SELECT i, i % 11, i % 97 FROM generate_series(1, 5000) i;
CREATE INDEX tc_idx ON tc USING sift_col (id, g, v) WITH (track_changes = 'ctid');
CREATE INDEX tk_idx ON tk USING sift_col (id, g, v) WITH (track_changes = 'id');
SELECT r::text, f.major || '.' || f.minor AS format FROM unnest(ARRAY['tc_idx', 'tk_idx']::regclass[]) r, LATERAL sift.index_format(r) f ORDER BY 1;

SET sift.test_create_format1 = off;
REINDEX INDEX CONCURRENTLY tc_idx;
REINDEX INDEX CONCURRENTLY tk_idx;
SELECT r::text, f.major || '.' || f.minor AS format FROM unnest(ARRAY['tc_idx', 'tk_idx']::regclass[]) r, LATERAL sift.index_format(r) f ORDER BY 1;

UPDATE tc SET v = v + 1 WHERE id % 10 = 0;
UPDATE tk SET v = v + 1 WHERE id % 10 = 0;
DELETE FROM tc WHERE id % 29 = 0;
DELETE FROM tk WHERE id % 29 = 0;
INSERT INTO tc SELECT i, i % 11, i % 97 FROM generate_series(5001, 5200) i;
INSERT INTO tk SELECT i, i % 11, i % 97 FROM generate_series(5001, 5200) i;
SELECT count(*) FROM tc;
SELECT count(*) FROM tk;
SELECT same('SELECT id, v FROM tc WHERE g = 3 AND v > 40') AS tc_same, same('SELECT id, v FROM tk WHERE g = 3 AND v > 40') AS tk_same;
VACUUM tc, tk;
SELECT same('SELECT id, v FROM tc WHERE g = 3 AND v > 40') AS tc_same, same('SELECT id, v FROM tk WHERE g = 3 AND v > 40') AS tk_same;

DROP TABLE tc, tk;
DROP FUNCTION same(text);
DROP EXTENSION pg_sift;
