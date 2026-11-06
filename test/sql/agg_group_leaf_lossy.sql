-- The group leaf a set-returning function reads (the jsonb array behind jsonb_array_elements, the object behind jsonb_each) is found by its attribute, its path and
-- its kind alone, with none of the tests a scalar leaf gets (not lossy, no '#' in its name): a group leaf is never lossy and has no name, and the virtual leaves
-- (document_present's '<col>#present', the members read as expressions) are scalars, which the kind keeps out. This pins it: an index with a lossy scalar leaf (char
-- without a length), the leaf of document_present and both group kinds gives the rows of the normal plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;

CREATE TABLE gl (id int PRIMARY KEY, g int, c bpchar, doc jsonb, m jsonb) WITH (autovacuum_enabled = false);
INSERT INTO gl SELECT i, i % 7, CASE i % 4 WHEN 0 THEN 'a' WHEN 1 THEN 'a  ' WHEN 2 THEN 'b' ELSE NULL END,
	CASE WHEN i % 11 = 0 THEN NULL WHEN i % 13 = 0 THEN '{}'
		 ELSE jsonb_build_object('items', jsonb_build_array(jsonb_build_object('tag', 't' || i % 5, 'n', i % 9), jsonb_build_object('tag', 't' || (i + 1) % 5, 'n', NULL),
															 jsonb_build_object('n', i % 3))) END,
	CASE WHEN i % 17 = 0 THEN NULL ELSE jsonb_build_object('x', i % 5 + 0.5, 'y', i % 3 + 0.25) END
	FROM generate_series(1, 2000) i;
SET client_min_messages = warning;
CREATE INDEX gl_col ON gl USING sift_col (id, g, c, doc, m) WITH (paths = 'doc.items[]{tag text, n int4}, m.* float8', document_present = on);
RESET client_min_messages;
VACUUM (FREEZE, ANALYZE) gl;

CREATE FUNCTION rows_of(q text, agg bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', agg::text, true);
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION uses_sift(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%SiftAgg%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

CREATE TABLE shapes (q text PRIMARY KEY, sql text);
INSERT INTO shapes VALUES
	('elements by tag', $q$SELECT e->>'tag' AS tag, count(*) AS n, sum((e->>'n')::int) AS s FROM gl, jsonb_array_elements(doc->'items') e GROUP BY 1$q$),
	('elements by the lossy column', $q$SELECT c, count(*) AS n, sum((e->>'n')::int) AS s FROM gl, jsonb_array_elements(doc->'items') e GROUP BY 1$q$),
	('elements of a filtered row set', $q$SELECT g, count(*) AS n FROM gl, jsonb_array_elements(doc->'items') e WHERE c = 'a' AND e->>'tag' = 't1' GROUP BY 1$q$),
	('exists over the elements', $q$SELECT g, count(*) AS n FROM gl WHERE EXISTS (SELECT 1 FROM jsonb_array_elements(doc->'items') e WHERE e->>'tag' = 't2') GROUP BY 1$q$),
	('document is null beside the elements', $q$SELECT count(*) FILTER (WHERE doc IS NULL) AS nulls, count(*) FILTER (WHERE doc = '{}') AS empty, count(*) AS n FROM gl$q$),
	('each of the keys', $q$SELECT key, count(*) AS n, sum(value::float8) AS s FROM gl, jsonb_each(m) GROUP BY 1$q$),
	('each by the lossy column', $q$SELECT c, count(*) AS n FROM gl, jsonb_each(m) GROUP BY 1$q$),
	('each of a filtered row set', $q$SELECT key, count(*) AS n FROM gl, jsonb_each(m) WHERE c IS NOT NULL AND g < 4 GROUP BY 1$q$),
	('elements of rows with a column', $q$SELECT g, count(*) AS n FROM gl, jsonb_array_elements(doc->'items') e WHERE c IS NOT NULL GROUP BY 1$q$);

SELECT q, uses_sift(sql) AS on_index, rows_of(sql, true) = rows_of(sql, false) AS same_rows FROM shapes ORDER BY q;

DROP TABLE shapes;
DROP FUNCTION rows_of(text, bool), uses_sift(text);
DROP TABLE gl;
DROP EXTENSION pg_sift;
