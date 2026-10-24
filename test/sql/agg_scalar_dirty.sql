-- A typed leaf of a path (`doc.v float8`) cannot store a value of another JSON type (a numeric string, a boolean, a container). A scan that reads
-- the path as `(doc ->> 'v')::T` or as the cast of `doc -> 'v'` reads the rows that hold one again from the heap, so that it answers, errors
-- included, as the normal plan: the row raises the error of the cast where PostgreSQL evaluates it (an aggregate argument or a grouping
-- expression of a row that reaches the aggregates) and never when a conjunct or a lookup drops the row, or when the cast runs for some rows only.
-- Every query runs with and without the pushdown; the rows, or the error, must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

\ir test/agg_helpers.sql

-- what the scan does, and whether the normal plan does the same
CREATE FUNCTION outcome(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text := runq(q, false); b text := runq(q, true);
BEGIN
	RETURN CASE WHEN a <> b THEN format('DIFF off=%s on=%s', left(a, 200), left(b, 200))
	            WHEN a LIKE 'ERR %' THEN 'same ' || left(a, 70) ELSE 'same rows' END;
END $$;

-- the rows the scan read again from the heap
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

CREATE TABLE s_t (id int PRIMARY KEY, grp int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO s_t
SELECT i, i % 5, jsonb_build_object('v', (i % 17) + 0.5, 'n', i % 9, 'ok', i % 2 = 0, 't', 'k' || (i % 4), 'w', i::bigint * 1000000000)
FROM generate_series(1, 3000) i;
-- documents that are no object (a key of them is NULL) and JSON nulls
UPDATE s_t SET doc = '[1, 2]' WHERE id % 100 = 1;
UPDATE s_t SET doc = '"text"' WHERE id % 100 = 2;
UPDATE s_t SET doc = '7' WHERE id % 100 = 3;
UPDATE s_t SET doc = jsonb_set(doc, '{v}', 'null') WHERE id % 100 = 4;
CREATE INDEX s_t_col ON s_t USING sift_col (id, grp, doc) WITH (paths = 'doc.v float8, doc.n int4, doc.ok bool, doc.t text, doc.w int8');
VACUUM ANALYZE s_t;

-- the leaves hold no value they cannot store but the documents that are no object: those rows are read again
CREATE TABLE s_q (n text PRIMARY KEY, q text);
INSERT INTO s_q VALUES
	('io float8',        'SELECT grp, sum((doc ->> ''v'')::float8), count((doc ->> ''v'')::float8) FROM s_t GROUP BY grp'),
	('cast float8',      'SELECT grp, sum((doc -> ''v'')::float8) FROM s_t GROUP BY grp'),
	('io int4',          'SELECT grp, sum((doc ->> ''n'')::int4), max((doc ->> ''n'')::int4) FROM s_t GROUP BY grp'),
	('io int8',          'SELECT grp, sum((doc ->> ''w'')::int8), min((doc ->> ''w'')::int8) FROM s_t GROUP BY grp'),
	('io bool',          'SELECT grp, count(*) FILTER (WHERE grp > 1), bool_and((doc ->> ''ok'')::bool) FROM s_t GROUP BY grp'),
	('key float8',       'SELECT (doc -> ''v'')::float8 k, count(*) FROM s_t GROUP BY 1'),
	('key io',           'SELECT (doc ->> ''n'')::int4 k, avg((doc ->> ''v'')::float8) FROM s_t GROUP BY 1'),
	('expression',       'SELECT grp, sum((doc ->> ''v'')::float8 * 2 + 1) FROM s_t GROUP BY grp'),
	('having',           'SELECT grp, count(*) FROM s_t GROUP BY grp HAVING max((doc ->> ''v'')::float8) > 10'),
	('text key beside',  'SELECT doc ->> ''t'' k, sum((doc ->> ''v'')::float8) FROM s_t GROUP BY 1'),
	('another column',   'SELECT grp, sum((doc ->> ''v'')::float8) FROM s_t WHERE grp <> 0 GROUP BY grp'),
	('no groups',        'SELECT sum((doc ->> ''v'')::float8), min((doc ->> ''n'')::int4) FROM s_t');
SELECT n, pushed(q) AS pushed, reread(q) AS reread, outcome(q) FROM s_q ORDER BY n;

-- numeric strings, which `->>` and a cast of the text convert (the cast of the jsonb value raises for them, below)
UPDATE s_t SET doc = jsonb_set(jsonb_set(doc, '{v}', '"8.5"'), '{n}', '"3"') WHERE jsonb_typeof(doc) = 'object' AND id % 33 = 5;
UPDATE s_t SET doc = jsonb_set(doc, '{ok}', '"yes"') WHERE jsonb_typeof(doc) = 'object' AND id % 29 = 5;
UPDATE s_t SET doc = jsonb_set(doc, '{w}', to_jsonb((id::bigint * 1000000000)::text)) WHERE jsonb_typeof(doc) = 'object' AND id % 31 = 5;
VACUUM ANALYZE s_t;

SELECT n, pushed(q) AS pushed, reread(q) AS reread, outcome(q) FROM s_q WHERE n NOT IN ('cast float8', 'key float8') ORDER BY n;

-- a value SQL cannot convert: the scan raises the error of the cast for the row it reaches, as the normal plan does
UPDATE s_t SET doc = jsonb_set(doc, '{v}', '"abc"') WHERE id = 1234;
UPDATE s_t SET doc = jsonb_set(doc, '{n}', '3.5') WHERE id = 1236;
UPDATE s_t SET doc = jsonb_set(doc, '{ok}', '"maybe"') WHERE id = 1238;
UPDATE s_t SET doc = jsonb_set(doc, '{w}', '1.5') WHERE id = 1240;
VACUUM ANALYZE s_t;
TRUNCATE s_q;
INSERT INTO s_q VALUES
	('io float8 raises',   'SELECT grp, sum((doc ->> ''v'')::float8) FROM s_t GROUP BY grp'),
	('cast float8 raises', 'SELECT grp, sum((doc -> ''v'')::float8) FROM s_t GROUP BY grp'),
	('io int4 raises',     'SELECT grp, sum((doc ->> ''n'')::int4) FROM s_t GROUP BY grp'),
	('io int8 raises',     'SELECT grp, sum((doc ->> ''w'')::int8) FROM s_t GROUP BY grp'),
	('io bool raises',     'SELECT grp, bool_and((doc ->> ''ok'')::bool) FROM s_t GROUP BY grp'),
	('key raises',         'SELECT (doc ->> ''v'')::float8 k, count(*) FROM s_t GROUP BY 1'),
	('having raises',      'SELECT grp, count(*) FROM s_t GROUP BY grp HAVING max((doc ->> ''v'')::float8) > 10'),
	-- the row is dropped by a condition on another column before the cast: nothing is raised
	('dropped by a conjunct', 'SELECT grp, sum((doc ->> ''v'')::float8) FROM s_t WHERE id <> 1234 GROUP BY grp'),
	('dropped by grp',     'SELECT grp, sum((doc ->> ''v'')::float8) FROM s_t WHERE grp <> 4 GROUP BY grp'),
	('dropped, cast',      'SELECT grp, sum((doc -> ''v'')::float8) FROM s_t WHERE grp <> 4 GROUP BY grp'),
	-- the leaf of another row is dirty, the one read is not
	('other leaf',         'SELECT grp, sum((doc ->> ''n'')::int4) FROM s_t WHERE id <> 1236 GROUP BY grp'),
	('clean leaf',         'SELECT grp, count(*) FILTER (WHERE doc ->> ''t'' = ''k1'') FROM s_t GROUP BY grp');
SELECT n, pushed(q) AS pushed, outcome(q) FROM s_q ORDER BY n;

-- the cast runs for some rows only, or the leaf is read otherwise: the normal plan, rows or error as the heap (a condition or a FILTER on the cast: agg_scalar_dirty_where, agg_scalar_dirty_filter)
TRUNCATE s_q;
INSERT INTO s_q VALUES
	('case branch',        'SELECT grp, sum(CASE WHEN grp = 1 THEN (doc ->> ''v'')::float8 END) FROM s_t GROUP BY grp'),
	('case branch drops',  'SELECT grp, sum(CASE WHEN id <> 1234 THEN (doc ->> ''v'')::float8 END) FROM s_t GROUP BY grp'),
	('two kinds',          'SELECT grp, sum((doc ->> ''v'')::float8), sum((doc -> ''v'')::float8) FROM s_t GROUP BY grp'),
	('typeof',             'SELECT grp, sum((doc ->> ''v'')::float8) FROM s_t WHERE jsonb_typeof(doc -> ''v'') <> ''string'' GROUP BY grp'),
	('sub-select',         'SELECT grp, sum((doc ->> ''v'')::float8) FROM s_t WHERE id IN (SELECT id FROM s_t WHERE (doc ->> ''v'')::float8 > 3) GROUP BY grp');
SELECT n, pushed(q) AS pushed, outcome(q) FROM s_q ORDER BY n;

-- rows the snapshot does not see raise nothing: the dirty rows are deleted, their units still hold them
DELETE FROM s_t WHERE id IN (1234, 1236, 1238, 1240);
SELECT n, pushed(q) AS pushed, outcome(q) FROM (VALUES
	('io float8 deleted',   'SELECT grp, sum((doc ->> ''v'')::float8) FROM s_t GROUP BY grp'),
	('cast float8 deleted', 'SELECT grp, sum((doc -> ''v'')::float8) FROM s_t GROUP BY grp'),
	('io int4 deleted',     'SELECT grp, sum((doc ->> ''n'')::int4) FROM s_t GROUP BY grp'),
	('only the deleted',    'SELECT grp, sum((doc -> ''v'')::float8) FROM s_t WHERE id IN (1234, 1236, 1238) GROUP BY grp')) v(n, q) ORDER BY n;
DROP TABLE s_q;
DROP TABLE s_t;
DROP FUNCTION reread(text);
DROP FUNCTION pushed(text);
DROP FUNCTION outcome(text);
DROP FUNCTION runq(text, bool), outcome, runs_in_scan;
DROP EXTENSION pg_sift;
