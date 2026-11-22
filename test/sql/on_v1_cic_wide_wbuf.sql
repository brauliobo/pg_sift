-- V1 sift_table: IN / NOT IN lists over wide leaves (numeric, interval) are decided by a binary search of the sorted distinct values, with the SQL meaning of NULL, and equal values
-- of different spellings (0.0 and 0.00, 12 hours and 0.5 day); a table with write buffer rows, updates and deletes gives the same rows with the source filter and without; and
-- CREATE INDEX CONCURRENTLY on a table whose index entries do not fit in maintenance_work_mem gives an index that holds every row.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE ow (id int, a int, n numeric(38, 6), iv interval, g int) USING sift_table;
INSERT INTO ow SELECT i, i % 40, CASE WHEN i % 11 = 0 THEN NULL ELSE (i % 30) * 0.5 END, make_interval(hours => i % 9 * 12), i % 5 FROM generate_series(1, 30000) i;
VACUUM (FREEZE, ANALYZE) ow;
INSERT INTO ow SELECT i, i % 40, (i % 30) * 0.5, make_interval(hours => i % 9 * 12), i % 5 FROM generate_series(30001, 32000) i;
UPDATE ow SET n = n + 1 WHERE id % 97 = 0;
DELETE FROM ow WHERE id % 211 = 0;

CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	EXECUTE 'SELECT md5(string_agg(x::text, ''|'' ORDER BY x::text)) FROM (' || q || ') x' INTO a;
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE 'SELECT md5(string_agg(x::text, ''|'' ORDER BY x::text)) FROM (' || q || ') x' INTO b;
	RETURN a IS NOT DISTINCT FROM b;
END $$;

CREATE FUNCTION in_source(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal';
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ '(\d+) wide filters in source' THEN r := 'wide'; END IF;
	END LOOP;
	RETURN r;
END $$;

CREATE TABLE ow_q (q text);
INSERT INTO ow_q VALUES
	('SELECT g, count(*) FROM ow WHERE n IN (0, 0.50, 1.5, 2, 3.5, 4, 5.5, 6, 7.5, 8, 9.5, 10, 0.00, 1.50) GROUP BY 1'),
	('SELECT g, count(*) FROM ow WHERE n NOT IN (0, 0.50, 1.5, 2, 3.5, 4, 5.5, 6, 7.5, 8, 9.5, 10, 0.00, 1.50) GROUP BY 1'),
	('SELECT g, count(*) FROM ow WHERE n IN (0, 0.50, 1.5, 2, 3.5, 4, 5.5, NULL, 7.5, 8) GROUP BY 1'),
	('SELECT g, count(*) FROM ow WHERE n NOT IN (0, 0.50, 1.5, 2, 3.5, 4, 5.5, NULL, 7.5, 8) GROUP BY 1'),
	('SELECT g, count(*) FROM ow WHERE n IN (3, 2, 1, 5, 4, 6, 9, 8, 7) AND a < 20 GROUP BY 1'),
	('SELECT g, count(*) FROM ow WHERE n NOT IN (3, 2, 1, 5, 4, 6, 9, 8, 7) AND a < 20 GROUP BY 1'),
	('SELECT g, count(*) FROM ow WHERE n IN (-1, -2, -3, -4, -5, -6) GROUP BY 1'),
	('SELECT g, count(*) FROM ow WHERE n NOT IN (-1, -2, -3, -4, -5, -6) GROUP BY 1'),
	('SELECT a, count(*) FROM ow WHERE iv IN (interval ''12 hours'', interval ''0.5 day'', interval ''1 day'', interval ''36 hours'', interval ''2 days'', interval ''72 hours'') GROUP BY 1'),
	('SELECT a, count(*) FROM ow WHERE iv NOT IN (interval ''12 hours'', interval ''0.5 day'', interval ''1 day'', interval ''36 hours'', interval ''2 days'', interval ''72 hours'') GROUP BY 1'),
	('SELECT a, count(*) FROM ow WHERE iv NOT IN (interval ''12 hours'', interval ''1 day'', interval ''36 hours'', interval ''2 days'', interval ''3 days'', NULL) GROUP BY 1'),
	('SELECT g, count(*) FROM ow WHERE n IN (1, 2, 3) GROUP BY 1'),
	('SELECT g, count(*) FROM ow WHERE n NOT IN (1, 2, 3, 4) GROUP BY 1');

SELECT regexp_replace(q, '^.* FROM ow WHERE ', '') AS q, in_source(q) AS in_source, same(q) AS same FROM ow_q ORDER BY q;

CREATE TABLE oc (id int, k int, pad int) USING sift_table;
INSERT INTO oc SELECT i, (i * 7919) % 200003, i % 13 FROM generate_series(1, 200000) i;
VACUUM (FREEZE) oc;
INSERT INTO oc SELECT i, (i * 7919) % 200003, i % 13 FROM generate_series(200001, 201000) i;
DELETE FROM oc WHERE id % 1000 = 0;
SET maintenance_work_mem = '1MB';
CREATE INDEX CONCURRENTLY oc_k ON oc (k);
SELECT indisvalid, indisready FROM pg_index WHERE indexrelid = 'oc_k'::regclass;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) AS indexed_rows, (SELECT count(*) FROM oc WHERE id > 0) AS rows_of_table FROM oc WHERE k >= 0;
SELECT count(*) AS probes_found FROM generate_series(1, 201000, 997) p JOIN oc ON oc.k = (p * 7919) % 200003 AND oc.id = p;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT (SELECT count(*) FROM oc WHERE k >= 0) = (SELECT count(*) FROM oc) AS same_count;

DROP TABLE ow, oc, ow_q;
DROP FUNCTION same(text), in_source(text);
DROP EXTENSION pg_sift;
