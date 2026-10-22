-- A typed member of the elements of a list (`doc.parts[]{c text, score float8}`) cannot store a value of another JSON type. A scan that reads it as
-- `(p ->> 'score')::float8`, p the element of jsonb_array_elements(doc -> 'parts'), reads the rows that hold one again from the heap, so that it
-- answers, errors included, as the normal plan: the element raises the error of the cast where it reaches the aggregate. The scan keeps one error
-- for a row, not for each of its elements, so a condition on the elements (which may drop the one that raises) keeps the normal plan.
-- Every query runs with and without the pushdown; the rows, or the error, must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

\ir test/agg_helpers.sql

CREATE FUNCTION outcome(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text := runq(q, false); b text := runq(q, true);
BEGIN
	RETURN CASE WHEN a <> b THEN format('DIFF off=%s on=%s', left(a, 200), left(b, 200))
	            WHEN a LIKE 'ERR %' THEN 'same ' || left(a, 70) ELSE 'same rows' END;
END $$;

CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE sql AS $$ SELECT CASE WHEN pushed(q) THEN 'scan' ELSE 'normal' END $$;

CREATE FUNCTION reread(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE l text; n bigint := 0;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'Rows Read Again From The Heap' THEN n := regexp_replace(l, '\D', '', 'g')::bigint; END IF;
	END LOOP;
	RETURN n;
EXCEPTION WHEN others THEN RETURN -1;
END $$;

CREATE TABLE e_t (id int PRIMARY KEY, grp int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO e_t
SELECT i, i % 5, jsonb_build_object('parts', CASE i % 7
	WHEN 0 THEN '[]'::jsonb
	WHEN 1 THEN jsonb_build_array(jsonb_build_object('c', 'a', 'score', (i % 11) + 0.5))
	WHEN 2 THEN jsonb_build_array(jsonb_build_object('c', 'a', 'score', (i % 11) + 0.5), jsonb_build_object('c', 'b', 'score', i % 5))
	WHEN 3 THEN jsonb_build_array(jsonb_build_object('c', 'b'), jsonb_build_object('c', 'a', 'score', i % 3))
	ELSE jsonb_build_array(jsonb_build_object('c', 'b', 'score', (i % 9) * 0.25), jsonb_build_object('c', 'a', 'score', 1.5), jsonb_build_object('c', 'b', 'score', i % 4)) END)
FROM generate_series(1, 3000) i;
CREATE INDEX e_t_col ON e_t USING sift_col (id, grp, doc) WITH (paths = 'doc.parts[]{c text, score float8}');
-- numeric strings in the first element of some rows
UPDATE e_t SET doc = jsonb_set(doc, '{parts,0,score}', to_jsonb((id % 11 + 0.5)::text)) WHERE id % 33 = 5 AND jsonb_array_length(doc -> 'parts') > 0 AND doc #> '{parts,0}' ? 'score';
VACUUM ANALYZE e_t;

CREATE TABLE e_q (n text PRIMARY KEY, q text);
INSERT INTO e_q VALUES
	('avg',             'SELECT grp, avg((p ->> ''score'')::float8) FROM e_t, jsonb_array_elements(doc -> ''parts'') p GROUP BY grp'),
	('sum, count',      'SELECT grp, sum((p ->> ''score'')::float8), count((p ->> ''score'')::float8), count(*) FROM e_t, jsonb_array_elements(doc -> ''parts'') p GROUP BY grp'),
	('cast of jsonb',   'SELECT grp, avg((p -> ''score'')::float8) FROM e_t, jsonb_array_elements(doc -> ''parts'') p GROUP BY grp'),
	('by the text',     'SELECT p ->> ''c'' c, avg((p ->> ''score'')::float8), max((p ->> ''score'')::float8) FROM e_t, jsonb_array_elements(doc -> ''parts'') p GROUP BY 1'),
	('as the key',      'SELECT (p ->> ''score'')::float8 k, count(*) FROM e_t, jsonb_array_elements(doc -> ''parts'') p GROUP BY 1'),
	('with a where',    'SELECT grp, avg((p ->> ''score'')::float8) FROM e_t, jsonb_array_elements(doc -> ''parts'') p WHERE grp <> 2 GROUP BY grp'),
	('having',          'SELECT grp, count(*) FROM e_t, jsonb_array_elements(doc -> ''parts'') p GROUP BY grp HAVING max((p ->> ''score'')::float8) > 3'),
	('no groups',       'SELECT avg((p ->> ''score'')::float8), count(*) FROM e_t, jsonb_array_elements(doc -> ''parts'') p'),
	('gated',           'SELECT grp, sum((p ->> ''score'')::float8) FILTER (WHERE grp = 1) FROM e_t, jsonb_array_elements(doc -> ''parts'') p GROUP BY grp');
SELECT n, plan_of(q) AS plan, reread(q) AS reread, outcome(q) FROM e_q ORDER BY n;

-- a value SQL cannot convert: the element raises where it reaches the aggregate, unless a condition of the WHERE drops its row
UPDATE e_t SET doc = jsonb_set(doc, '{parts,0,score}', '"abc"') WHERE id = 1234;
UPDATE e_t SET doc = jsonb_set(doc, '{parts,0,score}', 'true') WHERE id = 1235;
VACUUM ANALYZE e_t;
SELECT id, doc -> 'parts' FROM e_t WHERE id IN (1234, 1235);
TRUNCATE e_q;
INSERT INTO e_q VALUES
	('raises',            'SELECT grp, avg((p ->> ''score'')::float8) FROM e_t, jsonb_array_elements(doc -> ''parts'') p GROUP BY grp'),
	('cast of jsonb',     'SELECT grp, avg((p -> ''score'')::float8) FROM e_t, jsonb_array_elements(doc -> ''parts'') p GROUP BY grp'),
	('as the key',        'SELECT (p ->> ''score'')::float8 k, count(*) FROM e_t, jsonb_array_elements(doc -> ''parts'') p GROUP BY 1'),
	('dropped by id',     'SELECT grp, avg((p ->> ''score'')::float8) FROM e_t, jsonb_array_elements(doc -> ''parts'') p WHERE id NOT IN (1234, 1235) GROUP BY grp'),
	('dropped by grp',    'SELECT grp, avg((p ->> ''score'')::float8) FROM e_t, jsonb_array_elements(doc -> ''parts'') p WHERE grp NOT IN (4, 0) GROUP BY grp'),
	('kept by the where', 'SELECT grp, avg((p ->> ''score'')::float8) FROM e_t, jsonb_array_elements(doc -> ''parts'') p WHERE grp = 4 GROUP BY grp'),
	('gated, raises',     'SELECT grp, sum((p ->> ''score'')::float8) FILTER (WHERE grp = 4) FROM e_t, jsonb_array_elements(doc -> ''parts'') p GROUP BY grp'),
	('gated, dropped',    'SELECT grp, sum((p ->> ''score'')::float8) FILTER (WHERE grp = 1) FROM e_t, jsonb_array_elements(doc -> ''parts'') p GROUP BY grp'),
	('the text only',     'SELECT p ->> ''c'' c, count(*) FROM e_t, jsonb_array_elements(doc -> ''parts'') p GROUP BY 1');
SELECT n, plan_of(q) AS plan, outcome(q) FROM e_q ORDER BY n;

-- a condition on the elements, or a cast the FILTER takes per element: the normal plan, rows or error as the heap
TRUNCATE e_q;
INSERT INTO e_q VALUES
	('element condition', 'SELECT grp, avg((p ->> ''score'')::float8) FROM e_t, jsonb_array_elements(doc -> ''parts'') p WHERE p ->> ''c'' = ''a'' GROUP BY grp'),
	('on the cast',       'SELECT grp, count(*) FROM e_t, jsonb_array_elements(doc -> ''parts'') p WHERE (p ->> ''score'')::float8 > 1 GROUP BY grp'),
	('filter on the cast', 'SELECT grp, count(*) FILTER (WHERE (p ->> ''score'')::float8 > 1) FROM e_t, jsonb_array_elements(doc -> ''parts'') p GROUP BY grp'),
	('case',              'SELECT grp, sum(CASE WHEN p ->> ''c'' = ''a'' THEN (p ->> ''score'')::float8 END) FROM e_t, jsonb_array_elements(doc -> ''parts'') p GROUP BY grp'),
	('both ways',         'SELECT grp, avg((p ->> ''score'')::float8), avg((p -> ''score'')::float8) FROM e_t, jsonb_array_elements(doc -> ''parts'') p GROUP BY grp');
SELECT n, plan_of(q) AS plan, outcome(q) FROM e_q ORDER BY n;

SET sift.enable_agg = on;
SET sift.explain_refusals = on;
EXPLAIN (COSTS OFF) SELECT grp, avg((p ->> 'score')::float8) FROM e_t, jsonb_array_elements(doc -> 'parts') p WHERE p ->> 'c' = 'a' GROUP BY grp;
RESET sift.explain_refusals;

-- rows the snapshot does not see raise nothing: the dirty rows are deleted, their units still hold them
DELETE FROM e_t WHERE id IN (1234, 1235);
SELECT n, plan_of(q) AS plan, outcome(q) FROM (VALUES
	('deleted',     'SELECT grp, avg((p ->> ''score'')::float8) FROM e_t, jsonb_array_elements(doc -> ''parts'') p GROUP BY grp'),
	('deleted key', 'SELECT (p ->> ''score'')::float8 k, count(*) FROM e_t, jsonb_array_elements(doc -> ''parts'') p GROUP BY 1')) v(n, q) ORDER BY n;

-- a list that is no array raises where jsonb_array_elements reads it: the list itself is flagged, the normal plan answers
INSERT INTO e_t VALUES (5001, 1, '{"parts": {"c": "a"}}'), (5002, 2, '{"parts": 5}');
TRUNCATE e_q;
INSERT INTO e_q VALUES
	('object list',  'SELECT grp, avg((p ->> ''score'')::float8) FROM e_t, jsonb_array_elements(doc -> ''parts'') p GROUP BY grp'),
	('dropped',      'SELECT grp, avg((p ->> ''score'')::float8) FROM e_t, jsonb_array_elements(doc -> ''parts'') p WHERE id < 5000 GROUP BY grp');
SELECT n, plan_of(q) AS plan, outcome(q) FROM e_q ORDER BY n;

-- the workers of a parallel scan read the rows again and raise or not as the leader does
CREATE TABLE e_p (id int PRIMARY KEY, grp int, doc jsonb) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO e_p SELECT i, i % 5, jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('c', 'a', 'score', (i % 11) + 0.5), jsonb_build_object('c', 'b', 'score', i % 4)))
FROM generate_series(1, 20000) i;
CREATE INDEX e_p_col ON e_p USING sift_col (id, grp, doc) WITH (paths = 'doc.parts[]{c text, score float8}');
UPDATE e_p SET doc = jsonb_set(doc, '{parts,0,score}', to_jsonb((id % 11 + 0.5)::text)) WHERE id % 33 = 5;
UPDATE e_p SET doc = jsonb_set(doc, '{parts,1,score}', '"abc"') WHERE id = 15001;
VACUUM ANALYZE e_p;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SET seq_page_cost = 10000;
CREATE FUNCTION workers(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Workers Planned' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
SELECT n, workers(q) AS parallel, outcome(q) FROM (VALUES
	('dropped', 'SELECT grp, avg((p ->> ''score'')::float8) FROM e_p, jsonb_array_elements(doc -> ''parts'') p WHERE id <> 15001 GROUP BY grp'),
	('raises',  'SELECT grp, avg((p ->> ''score'')::float8) FROM e_p, jsonb_array_elements(doc -> ''parts'') p GROUP BY grp')) v(n, q) ORDER BY n;
DROP FUNCTION workers(text);
DROP TABLE e_p;
DROP TABLE e_q;
DROP TABLE e_t;
DROP FUNCTION reread(text);
DROP FUNCTION plan_of(text);
DROP FUNCTION pushed(text);
DROP FUNCTION outcome(text);
DROP FUNCTION runq(text, bool), outcome, runs_in_scan;
DROP EXTENSION pg_sift;
