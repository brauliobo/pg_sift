-- The integer filters of a statement are applied by the table source on the encoded chunks, as the sift_col index does (Filters In Source), and give the
-- rows of the normal plan: comparisons, ranges (two bounds are one filter), IN and NOT IN lists, NULL tests, on a table with write buffer rows,
-- tombstones, a numeric column holding NaN, and a column added after the first groups were written (a filter on it stays with the executor).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE tf (id int, a int, b int, c bigint, n numeric(10, 2), d date, g int) USING sift_table;
INSERT INTO tf SELECT i, i % 100, CASE WHEN i % 9 = 0 THEN NULL ELSE i % 7 END, i::bigint * 1000000000 % 7000000000, (i % 50) * 0.5, date '2024-01-01' + i % 90, i % 5 FROM generate_series(1, 60000) i;
UPDATE tf SET n = 'NaN' WHERE id % 1001 = 0;
VACUUM (FREEZE, ANALYZE) tf;

\ir test/agg_rows_helpers.sql
CREATE FUNCTION in_source(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal';
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Filters In Source: %' THEN r := trim(split_part(l, ':', 2)); END IF;
	END LOOP;
	RETURN r;
END $$;

CREATE TABLE tf_q (q text);
INSERT INTO tf_q VALUES
	('SELECT g, count(*), sum(c) FROM tf WHERE a = 17 GROUP BY 1'),
	('SELECT g, count(*) FROM tf WHERE a BETWEEN 10 AND 30 GROUP BY 1'),
	('SELECT g, count(*) FROM tf WHERE a > 90 AND a <> 95 GROUP BY 1'),
	('SELECT g, count(*) FROM tf WHERE b IN (1, 3, 5) GROUP BY 1'),
	('SELECT g, count(*) FROM tf WHERE b NOT IN (1, 3, 5) GROUP BY 1'),
	('SELECT g, count(*) FROM tf WHERE b IS NOT NULL AND a < 50 GROUP BY 1'),
	('SELECT g, count(*) FROM tf WHERE a < 80 AND b >= 2 AND c <> 0 GROUP BY 1'),
	('SELECT count(*), sum(a) FROM tf WHERE d >= date ''2024-02-01'' AND d < date ''2024-03-01'''),
	('SELECT g, count(*) FROM tf WHERE n > 10 GROUP BY 1'),
	('SELECT g, count(*) FROM tf WHERE n = 5 GROUP BY 1'),
	('SELECT g, count(*) FROM tf WHERE n <> 5 AND a < 20 GROUP BY 1'),
	('SELECT count(*) FROM tf WHERE a > 100'),
	('SELECT count(*) FROM tf WHERE a >= 0 AND b IS NOT NULL');

SELECT regexp_replace(q, '^.* FROM tf WHERE ', '') AS q, in_source(q) AS in_source, agg_rows(q, true) IS NOT DISTINCT FROM agg_rows(q, false) AS same FROM tf_q ORDER BY q;

-- write buffer rows and tombstones
INSERT INTO tf SELECT 60000 + i, i % 100, i % 7, i, i % 50, date '2024-01-01' + i % 90, i % 5 FROM generate_series(1, 4000) i;
DELETE FROM tf WHERE a BETWEEN 40 AND 45 AND id < 20000;
UPDATE tf SET a = 17, b = NULL WHERE id BETWEEN 30000 AND 30050;
SELECT regexp_replace(q, '^.* FROM tf WHERE ', '') AS q, in_source(q) AS in_source, agg_rows(q, true) IS NOT DISTINCT FROM agg_rows(q, false) AS same FROM tf_q ORDER BY q;

-- a column added later: the groups written before it hold it as NULLs, and as the default of ADD COLUMN .. DEFAULT
ALTER TABLE tf ADD COLUMN w int;
ALTER TABLE tf ADD COLUMN u int DEFAULT 7;
INSERT INTO tf SELECT 70000 + i, i % 100, i % 7, i, i % 50, date '2024-01-01', i % 5, i % 10, i % 3 FROM generate_series(1, 20000) i;
VACUUM tf;
DELETE FROM tf_q;
INSERT INTO tf_q VALUES
	('SELECT g, count(*) FROM tf WHERE w = 3 GROUP BY 1'),
	('SELECT g, count(*) FROM tf WHERE w IS NULL AND a < 10 GROUP BY 1'),
	('SELECT g, count(*) FROM tf WHERE u = 7 GROUP BY 1'),
	('SELECT g, count(*) FROM tf WHERE u IN (1, 2) AND a > 50 GROUP BY 1'),
	('SELECT g, count(*) FROM tf WHERE a < 50 AND b = 2 GROUP BY 1'),
	('SELECT g, count(*) FROM tf WHERE a < 50 AND w < 5 GROUP BY 1');
SELECT regexp_replace(q, '^.* FROM tf WHERE ', '') AS q, in_source(q) AS in_source, agg_rows(q, true) IS NOT DISTINCT FROM agg_rows(q, false) AS same FROM tf_q ORDER BY q;

DROP TABLE tf_q, tf;
DROP FUNCTION agg_pushed(text), agg_rows(text, bool), in_source(text);
DROP EXTENSION pg_sift;
