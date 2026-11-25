-- A uuid column of a sift_table is stored as a dictionary of 16-byte images, as in a sift_col index: the top-N reads the codes of the stored dictionary
-- ("stored dictionary" in the plan), and keys, aggregates and conjuncts give the rows of the normal plan, in column groups, in write buffers, after
-- DML, with NULLs, and in segments that lack the column.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;

CREATE TABLE ud (id int, g int, u uuid) USING sift_table;
INSERT INTO ud SELECT i, i % 7, CASE WHEN i % 11 = 0 THEN NULL ELSE md5((i % 3000)::text)::uuid END FROM generate_series(1, 30000) i;
VACUUM (FREEZE, ANALYZE) ud;
\ir test/agg_rows_helpers.sql

CREATE FUNCTION top_stored(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%stored dictionary%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

CREATE TABLE ud_q (q text);
INSERT INTO ud_q VALUES
	('SELECT u FROM ud ORDER BY u LIMIT 10'),
	('SELECT u, id FROM ud ORDER BY u DESC NULLS LAST, id LIMIT 10'),
	('SELECT g, count(*), count(DISTINCT u), min(u::text) FROM ud GROUP BY g'),
	('SELECT count(*) FROM ud WHERE u = md5(''7'')::uuid'),
	('SELECT count(*) FROM ud WHERE u > md5(''7'')::uuid AND u IS NOT NULL'),
	('SELECT u, count(*) FROM ud GROUP BY u ORDER BY 2 DESC, 1 LIMIT 5');
SELECT top_stored(q) AS stored, agg_pushed(q) AS pushed, agg_rows(q, true) = agg_rows(q, false) AS same FROM ud_q ORDER BY q;

-- write buffer rows and tombstones
INSERT INTO ud SELECT 30000 + i, i % 7, md5((i % 500 + 4000)::text)::uuid FROM generate_series(1, 2000) i;
DELETE FROM ud WHERE id BETWEEN 100 AND 400;
UPDATE ud SET u = md5('updated')::uuid WHERE id BETWEEN 1000 AND 1010;
SELECT top_stored(q) AS stored, agg_rows(q, true) = agg_rows(q, false) AS same FROM ud_q ORDER BY q;

-- a uuid column added later: the groups written before it hold it as NULLs
ALTER TABLE ud ADD COLUMN v uuid;
INSERT INTO ud SELECT 40000 + i, i % 7, md5(i::text)::uuid, md5((i % 50)::text)::uuid FROM generate_series(1, 5000) i;
VACUUM ud;
DELETE FROM ud_q;
INSERT INTO ud_q VALUES
	('SELECT v FROM ud ORDER BY v LIMIT 10'),
	('SELECT v, id FROM ud ORDER BY v DESC NULLS LAST, id LIMIT 10'),
	('SELECT g, count(v), count(DISTINCT v) FROM ud GROUP BY g'),
	('SELECT count(*) FROM ud WHERE v IS NULL'),
	('SELECT count(*) FROM ud WHERE v = md5(''3'')::uuid');
SELECT agg_rows(q, true) = agg_rows(q, false) AS same FROM ud_q ORDER BY q;

DROP TABLE ud_q, ud;
DROP FUNCTION agg_pushed(text), agg_rows(text, bool), top_stored(text);
DROP EXTENSION pg_sift;
