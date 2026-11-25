-- The argument of an aggregate with a FILTER is computed for the rows the FILTER keeps, as PostgreSQL does: a divisor of zero in a row the FILTER leaves out raises no error, in a row it keeps or in the
-- argument of an aggregate with no FILTER it does. The scan's rows equal those of PostgreSQL's plan.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE fa (id int, g int, b int);
INSERT INTO fa SELECT i, i % 5, i % 3 FROM generate_series(1, 5000) i;
CREATE INDEX fa_col ON fa USING sift_col (id, g, b);
VACUUM ANALYZE fa;

CREATE TABLE shapes (n int, q text);
INSERT INTO shapes VALUES
	(1, 'SELECT g, sum(id / g) FILTER (WHERE g <> 0) FROM fa GROUP BY g'),
	(2, 'SELECT sum(id / g) FILTER (WHERE g <> 0) FROM fa'),
	(3, 'SELECT g, sum(id / g) FILTER (WHERE g <> 0), sum(id / g) FILTER (WHERE g > 2) FROM fa GROUP BY g'),
	(4, 'SELECT b, sum(id / g) FILTER (WHERE g <> 0), count(*) FROM fa GROUP BY b'),
	(5, 'SELECT g, sum(id / g) FILTER (WHERE g <> 0), count(id / g) FILTER (WHERE g <> 0 AND b <> 0) FROM fa GROUP BY g'),
	(6, 'SELECT g, sum(id / g) FILTER (WHERE g <> 0), sum(id / g) FROM fa GROUP BY g'),
	(7, 'SELECT sum(id / g) FROM fa');

CREATE FUNCTION outcome(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; scan boolean := false; a text; b text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP scan := scan OR l ~ 'Custom Scan \(SiftAgg'; END LOOP;
	BEGIN
		EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO a;
	EXCEPTION WHEN OTHERS THEN a := SQLERRM; END;
	SET sift.enable_agg = off;
	BEGIN
		EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO b;
	EXCEPTION WHEN OTHERS THEN b := SQLERRM; END;
	RESET sift.enable_agg;
	RETURN CASE WHEN scan THEN 'scan' ELSE 'plain' END || CASE WHEN a = b THEN ' same' ELSE ' DIFF' END || CASE WHEN a ~ '^[0-9a-f]{32}$' THEN '' ELSE ' error: ' || a END;
END $$;

SELECT n, outcome(q) FROM shapes ORDER BY n;

DROP FUNCTION outcome(text);
DROP TABLE shapes, fa;
DROP EXTENSION pg_sift;
