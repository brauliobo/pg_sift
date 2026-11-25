-- jsonb_typeof(doc -> 'key') on the unified aggregate scan (sift.test_unified_agg) of Format 1 and on Format 2.0: as a FILTER, a WHERE, a group key, a CASE, over documents that are an object with and
-- without the key, a JSON null, a number, a string, a boolean, an array, no object at all, and SQL NULL. A FILTER on a type is PostgreSQL's function over the document datum, whose answer the scan
-- takes for each row it keeps: every statement is run with the node and without it (sift.enable_agg) and the answers are compared, on both formats.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
\set VERBOSITY terse
SET sift.agg_force = on;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

CREATE TABLE d (id int PRIMARY KEY, grp int, doc jsonb);
INSERT INTO d
SELECT i, i % 5,
	CASE i % 12
		WHEN 0 THEN jsonb_build_object('tag', 'a' || i, 'parts', jsonb_build_array(jsonb_build_object('kind', 'x', 'score', i % 7)), 'metrics', jsonb_build_object('m', i / 4.0))
		WHEN 1 THEN '{"tag": 5, "parts": "str"}'
		WHEN 2 THEN '{"tag": true, "parts": 7}'
		WHEN 3 THEN '{"tag": null, "parts": null}'
		WHEN 4 THEN '{}'
		WHEN 5 THEN NULL
		WHEN 6 THEN '[1, 2]'
		WHEN 7 THEN '5'
		WHEN 8 THEN '"str"'
		WHEN 9 THEN '{"tag": ["a"], "parts": {"kind": "x"}}'
		WHEN 10 THEN 'null'
		ELSE jsonb_build_object('tag', 'b' || i % 3, 'parts', jsonb_build_array(), 'metrics', jsonb_build_object('m', i / 8.0))
	END
FROM generate_series(1, 3000) i;

SET sift.test_create_format1 = on;
CREATE TABLE d1 (LIKE d INCLUDING ALL);
INSERT INTO d1 SELECT * FROM d;
CREATE INDEX d1_col ON d1 USING sift_col (id, grp, doc) WITH (paths = 'doc.tag text, doc.metrics.* float8, doc.parts[]{kind astext, score float8}');
SET sift.test_create_format1 = off;
CREATE TABLE d2 (LIKE d INCLUDING ALL);
INSERT INTO d2 SELECT * FROM d;
CREATE INDEX d2_col ON d2 USING sift_col (id, grp, doc) WITH (paths = 'doc.tag text, doc.metrics.* float8, doc.parts[]{kind astext, score float8}');
VACUUM (FREEZE, ANALYZE) d1;
VACUUM (FREEZE, ANALYZE) d2;

-- the node the plan of q has, and whether its rows are those of the heap (sift.enable_agg = off), in the order of the statement
CREATE FUNCTION check_q(q text) RETURNS TABLE (node text, same bool) LANGUAGE plpgsql AS $$
DECLARE l text; p text := ''; x text; y text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP p := p || l || E'\n'; END LOOP;
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''') FROM (%s) r', q) INTO x;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''') FROM (%s) r', q) INTO y;
	SET LOCAL sift.enable_agg = on;
	node := coalesce((regexp_match(p, 'Custom Scan \((Sift\w+)\)'))[1], 'none');
	same := x = y;
	RETURN NEXT;
END $$;

CREATE TABLE stmts (n int, q text);
INSERT INTO stmts VALUES
	(1, $$SELECT count(*) FILTER (WHERE jsonb_typeof(doc -> 'parts') = 'array') FROM @T$$),
	(2, $$SELECT count(*) FILTER (WHERE jsonb_typeof(doc -> 'parts') = 'string') FROM @T$$),
	(3, $$SELECT count(*) FILTER (WHERE jsonb_typeof(doc -> 'parts') = 'number') FROM @T$$),
	(4, $$SELECT count(*) FILTER (WHERE jsonb_typeof(doc -> 'parts') = 'object') FROM @T$$),
	(5, $$SELECT count(*) FILTER (WHERE jsonb_typeof(doc -> 'parts') = 'null') FROM @T$$),
	(6, $$SELECT count(*) FILTER (WHERE jsonb_typeof(doc -> 'parts') IS NULL) FROM @T$$),
	(7, $$SELECT count(*) FILTER (WHERE jsonb_typeof(doc -> 'parts') <> 'array') FROM @T$$),
	(8, $$SELECT count(*) FILTER (WHERE jsonb_typeof(doc -> 'tag') IN ('string', 'number')) FROM @T$$),
	(9, $$SELECT count(*) FILTER (WHERE jsonb_typeof(doc -> 'tag') = 'boolean'), count(*) FILTER (WHERE jsonb_typeof(doc -> 'tag') = 'array'), count(*) FROM @T$$),
	(10, $$SELECT count(*) FILTER (WHERE jsonb_typeof(doc -> 'absent') IS NULL), count(*) FILTER (WHERE jsonb_typeof(doc -> 'absent') = 'array') FROM @T$$),
	(11, $$SELECT count(*) FILTER (WHERE jsonb_typeof(doc) = 'object'), count(*) FILTER (WHERE jsonb_typeof(doc) = 'array'), count(*) FILTER (WHERE jsonb_typeof(doc) IS NULL) FROM @T$$),
	(12, $$SELECT sum(id) FILTER (WHERE jsonb_typeof(doc -> 'parts') = 'array' AND id > 100), count(*) FILTER (WHERE id > 5 OR jsonb_typeof(doc -> 'parts') = 'array') FROM @T$$),
	(13, $$SELECT grp, count(*) FILTER (WHERE jsonb_typeof(doc -> 'parts') = 'array'), sum(id) FILTER (WHERE jsonb_typeof(doc -> 'tag') = 'string') FROM @T GROUP BY 1$$),
	(14, $$SELECT jsonb_typeof(doc -> 'parts') t, count(*) FROM @T GROUP BY 1$$),
	(15, $$SELECT jsonb_typeof(doc -> 'tag') t, grp, count(*), sum(id) FROM @T GROUP BY 1, 2$$),
	(16, $$SELECT sum(CASE WHEN jsonb_typeof(doc -> 'parts') = 'array' THEN 1 ELSE 0 END), sum(CASE WHEN jsonb_typeof(doc -> 'tag') IS NULL THEN id END) FROM @T$$),
	(17, $$SELECT CASE WHEN jsonb_typeof(doc -> 'parts') = 'array' THEN 'many' WHEN jsonb_typeof(doc -> 'parts') IS NULL THEN 'none' ELSE 'one' END k, count(*) FROM @T GROUP BY 1$$),
	(18, $$SELECT count(*) FROM @T WHERE jsonb_typeof(doc -> 'parts') = 'array'$$),
	(19, $$SELECT count(*) FROM @T WHERE jsonb_typeof(doc -> 'parts') IS NULL$$),
	(20, $$SELECT grp, count(*) FROM @T WHERE jsonb_typeof(doc -> 'tag') <> 'string' GROUP BY 1$$),
	(21, $$SELECT count(*) FROM @T WHERE jsonb_typeof(doc -> 'tag') = 'string' OR jsonb_typeof(doc -> 'parts') = 'object'$$),
	(22, $$SELECT count(*) FILTER (WHERE jsonb_typeof(doc -> 'parts') = 'array') FROM @T WHERE grp = 2$$);

SELECT t, n, (check_q(replace(q, '@T', t))).* FROM unnest(ARRAY['d1', 'd2']) t, stmts ORDER BY t, n;

-- the counts themselves, of the table on Format 1 against the heap
SELECT count(*) FILTER (WHERE jsonb_typeof(doc -> 'parts') = 'array') AS arrays, count(*) FILTER (WHERE jsonb_typeof(doc -> 'parts') IS NULL) AS nulls FROM d1;
SET sift.enable_agg = off;
SELECT count(*) FILTER (WHERE jsonb_typeof(doc -> 'parts') = 'array') AS arrays, count(*) FILTER (WHERE jsonb_typeof(doc -> 'parts') IS NULL) AS nulls FROM d1;

RESET sift.enable_agg;
DROP TABLE d, d1, d2, stmts;
DROP FUNCTION check_q;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
