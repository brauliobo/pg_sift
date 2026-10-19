-- SiftAgg over a sift_col index with nothing for the source to filter (no integer conjunct): the scan starts with an empty filter set. It
-- is a SANITIZE-lane test: the scan once copied that empty set from a null pointer (memcpy of a null array, undefined behaviour), which
-- the sanitizer build aborts on and a normal build ignores. Every query runs with and without the pushdown; the results must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;

CREATE TABLE nf (id int, g int, n int, s text);
INSERT INTO nf SELECT i, i % 7, i % 100, 'v' || i % 13 FROM generate_series(1, 20000) i;
CREATE INDEX nf_col ON nf USING sift_col (id, g, n, s);
VACUUM ANALYZE nf;

CREATE FUNCTION agg_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION agg_pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%SiftAgg%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

CREATE TABLE agg_q (q text);
INSERT INTO agg_q VALUES
	('SELECT count(*), sum(n), min(id), max(id) FROM nf'),
	('SELECT g, count(*), sum(n) FROM nf GROUP BY g'),
	('SELECT s, count(*) FROM nf GROUP BY s'),
	('SELECT g, count(DISTINCT s) FROM nf GROUP BY g');

SELECT query, pushed, same FROM (SELECT q AS query, agg_pushed(q) AS pushed, agg_rows(q, true) IS NOT DISTINCT FROM agg_rows(q, false) AS same FROM agg_q) z ORDER BY query COLLATE "C";

DROP TABLE nf, agg_q;
DROP FUNCTION agg_rows, agg_pushed;
DROP EXTENSION pg_sift;
