-- The delta tail of a deletion vector (src/col/f2/del/delvec_delta.h) holds at most SIFT_DV_DELTA_MAX_MARKS marks in a chunk: a DELETE that kills more rows of one granule than that, once an
-- earlier DELETE gave the part a vector that takes deltas, writes the marks of the granule as several chunks. The vector reads back whole, VACUUM does not quarantine the index, and the
-- aggregate and the heap agree before and after it.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET sift.test_create_format1 = off;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE dv (id int PRIMARY KEY, g int, v int) WITH (autovacuum_enabled = false);
INSERT INTO dv SELECT i, i % 20, i % 1000 FROM generate_series(1, 100000) i;
CREATE INDEX dv_col ON dv USING sift_col (id, g, v);
VACUUM dv;

CREATE FUNCTION dv_state() RETURNS TABLE (dead bigint, quarantined boolean) LANGUAGE sql AS $$
	SELECT (SELECT sum(dead_rows) FROM sift.parts('dv_col')), (SELECT q.quarantined FROM sift._quarantine_state('dv_col'::regclass) q) $$;

CREATE FUNCTION dv_same() RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	SELECT format('%s %s %s', count(*), sum(v), sum(g)) INTO a FROM dv;
	SET LOCAL sift.enable_agg = off;
	SELECT format('%s %s %s', count(*), sum(v), sum(g)) INTO b FROM dv;
	RETURN CASE WHEN a = b THEN 'same ' ELSE 'DIFFERENT ' END || b;
END $$;

-- the first DELETE leaves the part a vector that takes deltas, the second kills 3000 rows of the granule it shares with the first, the third more rows than a chunk holds in one granule
DELETE FROM dv WHERE id BETWEEN 10 AND 700;
DELETE FROM dv WHERE id BETWEEN 701 AND 3700;
DELETE FROM dv WHERE id BETWEEN 3701 AND 8000;
SELECT * FROM dv_state();
SELECT dv_same();

VACUUM dv;
SELECT * FROM dv_state();
SELECT dv_same();

-- a granule killed whole by deltas
DELETE FROM dv WHERE id BETWEEN 8001 AND 8100;
DELETE FROM dv WHERE id BETWEEN 8101 AND 16000;
SELECT dv_same();
VACUUM dv;
SELECT * FROM dv_state();
SELECT dv_same();

-- a DELETE rolled back leaves its deleter aborted in the vector: the DELETEs after it rewrite the granules they touch over it and copy the others
BEGIN;
DELETE FROM dv WHERE id BETWEEN 20001 AND 26000;
ROLLBACK;
DELETE FROM dv WHERE id BETWEEN 24001 AND 40000;
SELECT dv_same();
DELETE FROM dv WHERE id % 7 = 0 AND id > 30000;
SELECT * FROM dv_state();
SELECT dv_same();
VACUUM dv;
SELECT * FROM dv_state();
SELECT dv_same();

DROP TABLE dv;
DROP FUNCTION dv_state();
DROP FUNCTION dv_same();
DROP EXTENSION pg_sift;
