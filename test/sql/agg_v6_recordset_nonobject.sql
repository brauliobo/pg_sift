-- jsonb_to_recordset over an array of objects that an index keeps shredded (a column of its own, or a key of a document), when one row's document is not one: an array with an element that is no
-- object (a number, a string, a boolean, null), a scalar, an object. PostgreSQL raises ("argument of jsonb_to_recordset must be an array of objects", "cannot call jsonb_to_recordset on a
-- non-array") and so must the aggregate scan, whichever index holds the leaves: Format 2 sift_col, Format 2 sift_table, and Format 1 on the unified scan (sift.test_unified_agg).
-- Each statement runs with the node and without it (sift.enable_agg); the answers, or the errors, are compared.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
\set VERBOSITY terse
SET sift.agg_force = on;
SET max_parallel_workers_per_gather = 0;
CREATE SCHEMA rn;
SET search_path = rn, public;

CREATE TABLE variants (n int, v text);
INSERT INTO variants VALUES
	(1, '[1, 2]'), (2, '"s"'), (3, '5'), (4, 'true'), (5, '[{"kind": "a"}, 2]'), (6, '[{"kind": "a"}, "s"]'), (7, '[{"kind": "a"}, null]'), (8, '[[1]]'),
	(9, '{"kind": "a"}'), (10, '[null]'), (11, '[true]'), (12, '[{"kind": "a"}, true]'), (13, '[{"kind": "a"}, [1]]'), (14, '["x", "y"]'), (15, '[1.5]');

CREATE TABLE forms (f text, q text);
INSERT INTO forms VALUES
	('column', 'SELECT e.kind, count(*) FROM @T f CROSS JOIN LATERAL jsonb_to_recordset(f.a) AS e(kind text, n int) GROUP BY 1'),
	('key', 'SELECT e.kind, count(*) FROM @T f CROSS JOIN LATERAL jsonb_to_recordset(f.b -> ''k'') AS e(kind text, n int) GROUP BY 1'),
	('left', 'SELECT e.kind, count(*) FROM @T f LEFT JOIN LATERAL jsonb_to_recordset(f.a) AS e(kind text, n int) ON true GROUP BY 1'),
	('total', 'SELECT count(*), sum(e.n) FROM @T f CROSS JOIN LATERAL jsonb_to_recordset(f.a) AS e(kind text, n int)'),
	('exists', 'SELECT count(*) FROM @T f WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(f.a) AS e(kind text, n int) WHERE e.kind = ''a'')');

-- the rows of a table of a lane and a variant: 4,000 of one object each, the document of row 2,000 is the variant
CREATE FUNCTION fill(t text, v text) RETURNS text LANGUAGE sql AS $$
	SELECT format($f$INSERT INTO %s SELECT i, CASE WHEN i = 2000 THEN %L ELSE '[{"kind": "a", "n": 1}]' END::jsonb,
		jsonb_build_object('k', CASE WHEN i = 2000 THEN %L ELSE '[{"kind": "b", "n": 2}]' END::jsonb) FROM generate_series(1, 4000) i$f$, t, v, v) $$;

CREATE FUNCTION outcome(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT coalesce(md5(string_agg(x::text, ''|'' ORDER BY x::text)), ''empty'') FROM (%s) x', q) INTO r;
	RETURN r;
EXCEPTION WHEN OTHERS THEN RETURN 'ERROR: ' || SQLERRM;
END $$;

-- the node the plan has, whether the answer (or the error) with it is the one without it, and the error
CREATE FUNCTION check_q(q text) RETURNS TABLE (node text, same bool, err text) LANGUAGE plpgsql AS $$
DECLARE l text; p text := ''; a text; b text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP p := p || l || E'\n'; END LOOP;
	a := outcome(q);
	SET LOCAL sift.enable_agg = off;
	b := outcome(q);
	SET LOCAL sift.enable_agg = on;
	node := coalesce((regexp_match(p, 'Custom Scan \((Sift\w+)\)'))[1], 'none');
	same := a = b;
	err := CASE WHEN b LIKE 'ERROR:%' THEN b ELSE 'none' END;
	RETURN NEXT;
END $$;

-- the report of a lane: for each form, the variants run, those whose plan has a Sift node, and those whose answer or error is not the heap's
CREATE FUNCTION lane_report(lane text) RETURNS TABLE (form text, variants int, on_node int, differ int, differing text) LANGUAGE sql AS $$
	SELECT f.f, count(*)::int, (count(*) FILTER (WHERE c.node <> 'none'))::int, (count(*) FILTER (WHERE NOT c.same))::int,
		coalesce(string_agg(v.n::text, ',' ORDER BY v.n) FILTER (WHERE NOT c.same), '')
	FROM forms f, variants v, LATERAL check_q(replace(f.q, '@T', format('rt_%s_%s', lane, v.n))) c
	GROUP BY f.f ORDER BY f.f $$;

-- what PostgreSQL says of each variant
SELECT v.n, v.v, c.err FROM variants v, LATERAL (SELECT outcome(format('SELECT count(*) FROM jsonb_to_recordset(%L::jsonb) AS e(kind text, n int)', v.v)) AS err) c ORDER BY v.n;

-- Format 2 sift_col
SET sift.test_create_format1 = off;
SELECT format('CREATE TABLE rt_c_%s (id int, a jsonb, b jsonb) WITH (autovacuum_enabled = false)', n) FROM variants \gexec
SELECT fill(format('rt_c_%s', n), v) FROM variants \gexec
SELECT format($f$CREATE INDEX rt_c_%1$s_col ON rt_c_%1$s USING sift_col (id, a, b) WITH (paths = 'a[]{kind text, n int4}, b.k[]{kind text, n int4}', rollup_columns = 'off')$f$, n) FROM variants \gexec
SELECT format('VACUUM (FREEZE, ANALYZE) rt_c_%s', n) FROM variants \gexec
SELECT * FROM lane_report('c');

-- Format 2 sift_table
SELECT format('CREATE TABLE rt_t_%s (id int, a jsonb, b jsonb) USING sift_table', n) FROM variants \gexec
SELECT format($f$DO $d$ BEGIN PERFORM sift.table_set_paths('rt_t_%1$s', 'a', 'a[]{kind text, n int4}'), sift.table_set_paths('rt_t_%1$s', 'b', 'b->k[]{kind text, n int4}'); END $d$$f$, n) FROM variants \gexec
SELECT fill(format('rt_t_%s', n), v) FROM variants \gexec
SELECT format('VACUUM rt_t_%s', n) FROM variants \gexec
SELECT * FROM lane_report('t');

-- Format 1 on the unified scan
SET sift.test_create_format1 = on;
SET sift.test_unified_agg = on;
SELECT format('CREATE TABLE rt_f_%s (id int, a jsonb, b jsonb) WITH (autovacuum_enabled = false)', n) FROM variants \gexec
SELECT fill(format('rt_f_%s', n), v) FROM variants \gexec
SELECT format($f$CREATE INDEX rt_f_%1$s_col ON rt_f_%1$s USING sift_col (id, a, b) WITH (paths = 'a[]{kind text, n int4}, b.k[]{kind text, n int4}', rollup_columns = 'off')$f$, n) FROM variants \gexec
SELECT format('VACUUM (FREEZE, ANALYZE) rt_f_%s', n) FROM variants \gexec
SELECT * FROM lane_report('f');

-- the variants as new versions of stored rows (Format 2 sift_col)
SET sift.test_create_format1 = off;
CREATE TABLE rw (id int, a jsonb, b jsonb) WITH (autovacuum_enabled = false);
INSERT INTO rw SELECT i, '[{"kind": "a", "n": 1}]', '{"k": [{"kind": "b", "n": 2}]}' FROM generate_series(1, 4000) i;
CREATE INDEX rw_col ON rw USING sift_col (id, a, b) WITH (paths = 'a[]{kind text, n int4}, b.k[]{kind text, n int4}', rollup_columns = 'off');
VACUUM (FREEZE, ANALYZE) rw;
CREATE FUNCTION rewrite_report() RETURNS TABLE (n int, v text, node text, same bool, err text) LANGUAGE plpgsql AS $$
DECLARE r record;
BEGIN
	FOR r IN SELECT * FROM variants ORDER BY variants.n LOOP
		UPDATE rw SET a = r.v::jsonb, b = jsonb_build_object('k', r.v::jsonb) WHERE id = 2000;
		SELECT r.n, r.v, c.node, c.same, c.err INTO n, v, node, same, err FROM check_q('SELECT e.kind, count(*) FROM rw f CROSS JOIN LATERAL jsonb_to_recordset(f.a) AS e(kind text, n int) GROUP BY 1') c;
		RETURN NEXT;
		SELECT r.n, r.v, c.node, c.same, c.err INTO n, v, node, same, err FROM check_q('SELECT e.kind, count(*) FROM rw f CROSS JOIN LATERAL jsonb_to_recordset(f.b -> ''k'') AS e(kind text, n int) GROUP BY 1') c;
		RETURN NEXT;
	END LOOP;
END $$;
SELECT count(*) AS checks, count(*) FILTER (WHERE node <> 'none') AS on_node, count(*) FILTER (WHERE NOT same) AS differ, string_agg(n::text, ',' ORDER BY n) FILTER (WHERE NOT same) AS differing FROM rewrite_report();
RESET search_path;
DROP SCHEMA rn CASCADE;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
