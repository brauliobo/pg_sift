-- sift.json_build_object writes the bytes of json_build_object, for every type of argument, key and error; its type lookups are made once per call
-- site and made again after a change of a type or a cast
CREATE EXTENSION pg_sift;
SET TimeZone = 'UTC';
SET DateStyle = 'ISO, MDY';
SET extra_float_digits = 1;

CREATE TYPE jb_pair AS (a int, b text);
CREATE TYPE jb_nest AS (p jb_pair, q int[], r jb_pair[]);
CREATE TYPE jb_lvl3 AS ENUM ('lo', 'ok', 'hi');
CREATE DOMAIN jb_posint AS int CHECK (VALUE > 0);
CREATE DOMAIN jb_label AS text;
CREATE DOMAIN jb_doc AS json;
CREATE TYPE jb_opaque AS ENUM ('o1', 'o2');
CREATE FUNCTION jb_opaque_json(jb_opaque) RETURNS json LANGUAGE sql IMMUTABLE AS $$ SELECT json_build_object('opaque', $1::text) $$;
CREATE CAST (jb_opaque AS json) WITH FUNCTION jb_opaque_json(jb_opaque);

-- what each of the two functions gives for an argument list: the text, or the error with its code, message and hint
CREATE FUNCTION jb_run(fn text, args text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; st text; m text; h text;
BEGIN
	EXECUTE format('SELECT %s(%s)::text', fn, args) INTO r;
	RETURN coalesce(r, 'NULL');
EXCEPTION WHEN OTHERS THEN
	GET STACKED DIAGNOSTICS st = RETURNED_SQLSTATE, m = MESSAGE_TEXT, h = PG_EXCEPTION_HINT;
	RETURN format('error %s %s [%s]', st, m, h);
END $$;
CREATE FUNCTION jb_same(args text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text := jb_run('json_build_object', args); b text := jb_run('sift.json_build_object', args);
BEGIN
	RETURN CASE WHEN a = b THEN 'same ' || a ELSE format('DIFFERENT core %s sift %s', a, b) END;
END $$;

SELECT jb_same(a) AS result FROM unnest(ARRAY[
	-- the arity and the keys
	$$'a', 1$$, $$'a', 1, 'b', 2, 'a', 3$$, $$'a'$$, $$'a', 1, 'b'$$, $$NULL, 1$$, $$NULL::text, 1$$, $$'a', NULL$$, $$'a', NULL::int, 'b', NULL::json$$,
	$$VARIADIC ARRAY['a', '1', 'b', '2']$$, $$VARIADIC NULL::text[]$$, $$VARIADIC ARRAY[]::text[]$$, $$VARIADIC ARRAY[1, 2, 3]$$, $$VARIADIC '{}'::int[]$$,
	-- text, escapes, unicode
	$$'k', 'plain'$$, $$'k', 'quo"te'$$, $$'k', E'back\\slash'$$, $$'k', E'tab\tnl\ncr\r\bff\f'$$, $$'k', E'\u0001\u001f\u007f'$$, $$'k', 'ünï©ödé ✓ 😀'$$, $$'k', ''$$, $$'k', '</script>'$$,
	$$'k', 'v'::varchar$$, $$'k', 'v'::char(5)$$, $$'k', 'v'::name$$, $$'k', 'v'::"char"$$, $$'k', '\x00ff'::bytea$$, $$'k', 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'::uuid$$,
	$$'k"ey', 1, E'k\\ey', 2, 'ké', 3, '', 4$$,
	-- numbers
	$$'k', 1::int2$$, $$'k', -1::int4$$, $$'k', 9223372036854775807::int8$$, $$'k', 1.5::float4$$, $$'k', 0.1::float8$$, $$'k', -0.0::float8$$, $$'k', 1e100::float8$$,
	$$'k', 'NaN'::float8$$, $$'k', 'Infinity'::float8$$, $$'k', '-Infinity'::float4$$, $$'k', 1.50::numeric$$, $$'k', -0::numeric$$, $$'k', 'NaN'::numeric$$,
	$$'k', 123456789012345678901234567890.123456789::numeric$$, $$'k', 1e-30::numeric$$, $$'k', 12.5::money$$, $$'k', 5::oid$$, $$'k', 'pg_class'::regclass$$,
	-- booleans, dates and times
	$$'k', true, 'l', false$$, $$'k', DATE '2026-02-03'$$, $$'k', DATE 'infinity'$$, $$'k', DATE '0044-03-15 BC'$$, $$'k', TIMESTAMP '2026-02-03 04:05:06.789'$$,
	$$'k', TIMESTAMPTZ '2026-02-03 04:05:06.789+05:30'$$, $$'k', TIMESTAMPTZ '-infinity'$$, $$'k', TIME '04:05:06'$$, $$'k', TIMETZ '04:05:06+02'$$, $$'k', INTERVAL '1 year 2 mons 3 days 04:05:06'$$,
	-- json, jsonb, arrays, composites, records
	$$'k', '{"a" :   [1,  2]}'::json$$, $$'k', '{"a" :   [1,  2]}'::jsonb$$, $$'k', 'null'::json$$, $$'k', '"s"'::jsonb$$, $$'k', jb_doc '{"x" :  1}'$$,
	$$'k', ARRAY[1, 2, NULL]$$, $$'k', ARRAY['a', 'b"c', NULL]$$, $$'k', ARRAY[[1, 2], [3, 4]]$$, $$'k', '{}'::int[]$$, $$'k', ARRAY[DATE '2026-01-01']$$, $$'k', ARRAY['{"a": 1}'::json]$$,
	$$'k', ROW(1, 'x')$$, $$'k', ROW(1, 'x')::jb_pair$$, $$'k', ROW(NULL, NULL)::jb_pair$$, $$'k', ROW(ROW(1, 'x'), ARRAY[1, 2], ARRAY[ROW(2, 'y')::jb_pair])::jb_nest$$,
	$$'k', ARRAY[ROW(1, 'x')::jb_pair, NULL]$$, $$'k', (SELECT t FROM (VALUES (1, 'a')) t (x, y))$$,
	-- domains, enums, casts, other types
	$$'k', 5::jb_posint$$, $$'k', 'lab"el'::jb_label$$, $$'k', 'ok'::jb_lvl3$$, $$'k', ARRAY['lo', 'hi']::jb_lvl3[]$$, $$'k', 'o1'::jb_opaque$$, $$'k', ARRAY['o1'::jb_opaque]$$,
	$$'k', '1.2.3.4/24'::inet$$, $$'k', '10.0.0.0/8'::cidr$$, $$'k', '08:00:2b:01:02:03'::macaddr$$, $$'k', B'1011'::varbit$$, $$'k', int4range(1, 5)$$, $$'k', daterange('2026-01-01', NULL)$$,
	$$'k', 'a:1 b:2'::tsvector$$, $$'k', '<a>1</a>'::xml$$, $$'k', point(1, 2)$$, $$'k', '1 2 3'::int2vector$$,
	-- keys that are not text
	$$1, 'v'$$, $$1.5::numeric, 'v'$$, $$true, 'v'$$, $$DATE '2026-02-03', 'v'$$, $$TIMESTAMPTZ '2026-02-03 04:05:06+00', 'v'$$, $$5::jb_posint, 'v'$$, $$'ok'::jb_lvl3, 'v'$$,
	$$ARRAY[1], 'v'$$, $$ROW(1, 'x'), 'v'$$, $$'{"a": 1}'::json, 'v'$$, $$'{"a": 1}'::jsonb, 'v'$$, $$'o1'::jb_opaque, 'v'$$, $$'NaN'::numeric, 'v'$$, $$'NaN'::float8, 'v'$$,
	-- unknown literals and nesting
	$$'a', 'b', 'c', NULL, 'd', 'e'$$, $$'a', json_build_object('b', json_build_object('c', 1))$$, $$'a', sift.json_build_object('b', ARRAY[sift.json_build_object('c', 1)])$$
]) a;

-- another TimeZone and DateStyle
SET TimeZone = 'Asia/Kolkata';
SET DateStyle = 'German, DMY';
SELECT jb_same(a) AS result FROM unnest(ARRAY[
	$$'k', TIMESTAMPTZ '2026-02-03 04:05:06.789+00', 'l', TIMESTAMP '2026-02-03 04:05:06', 'm', DATE '2026-02-03'$$,
	$$'k', ARRAY[TIMESTAMPTZ '2026-02-03 04:05:06+00']$$, $$'k', ROW(DATE '2026-02-03', INTERVAL '3 days')$$, $$TIMESTAMPTZ '2026-02-03 04:05:06+00', 1$$
]) a;
RESET TimeZone;
RESET DateStyle;

-- rows: the lookups are made once for a column of one type, and again when the type of an argument changes between calls of one call site
CREATE TABLE jb_rows (id int, t text, n numeric, d date, j jsonb, anyv text);
INSERT INTO jb_rows SELECT i, CASE WHEN i % 7 = 0 THEN NULL ELSE 'v"' || i END, i / 3.0, DATE '2026-01-01' + i, jsonb_build_object('i', i), CASE WHEN i % 2 = 0 THEN i::text ELSE NULL END FROM generate_series(1, 200) i;
SELECT count(*) AS rows, bool_and(json_build_object('id', id, 't', t, 'n', n, 'd', d, 'j', j, 'r', ROW(id, t)::jb_pair)::text
	= sift.json_build_object('id', id, 't', t, 'n', n, 'd', d, 'j', j, 'r', ROW(id, t)::jb_pair)::text) AS same FROM jb_rows;
SELECT count(*) AS groups, bool_and(a = b) AS same FROM (
	SELECT json_agg(json_build_object('id', id, 't', t))::text AS a, json_agg(sift.json_build_object('id', id, 't', t))::text AS b FROM jb_rows GROUP BY id % 5) s;
-- one call site, polymorphic argument of different types in turn
CREATE FUNCTION jb_poly(anyelement) RETURNS TABLE (core text, sift text) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT json_build_object('v', $1)::text, sift.json_build_object('v', $1)::text;
END $$;
SELECT core = sift AS same, sift AS result FROM (SELECT (jb_poly(v)).* FROM (VALUES (1), (2)) t (v)) a;
SELECT core = sift AS same, sift AS result FROM (SELECT (jb_poly(v)).* FROM (VALUES ('a'::text), ('b"')) t (v)) a;
SELECT core = sift AS same, sift AS result FROM jb_poly(DATE '2026-02-03') a;
SELECT core = sift AS same, sift AS result FROM jb_poly(ARRAY[1, 2]) a;

-- a prepared statement keeps its call site: a cast created or dropped under it is met on the next execution
CREATE TYPE jb_late AS ENUM ('x', 'y');
PREPARE jb_p(jb_late) AS SELECT json_build_object('v', $1)::text = sift.json_build_object('v', $1)::text AS same, sift.json_build_object('v', $1) AS result;
EXECUTE jb_p('x');
CREATE FUNCTION jb_late_json(jb_late) RETURNS json LANGUAGE sql IMMUTABLE AS $$ SELECT json_build_object('late', $1::text) $$;
CREATE CAST (jb_late AS json) WITH FUNCTION jb_late_json(jb_late);
EXECUTE jb_p('x');
DROP CAST (jb_late AS json);
EXECUTE jb_p('x');
CREATE DOMAIN jb_dom AS text;
PREPARE jb_q(jb_dom) AS SELECT json_build_object('v', $1)::text = sift.json_build_object('v', $1)::text AS same, sift.json_build_object('v', $1) AS result;
EXECUTE jb_q('a');
ALTER TYPE jb_lvl3 ADD VALUE 'ecstatic';
SELECT jb_same($$'k', 'ecstatic'::jb_lvl3$$) AS result;
DEALLOCATE jb_p;
DEALLOCATE jb_q;

-- the cast of a type to json shows in the text, and is dropped from it again
-- the planner treats it as a function of the session: stable, parallel safe, not strict
SELECT pronamespace::regnamespace AS schema, provolatile, proparallel, proisstrict, prorettype::regtype, provariadic::regtype
FROM pg_proc WHERE proname = 'json_build_object' AND pronamespace IN ('sift'::regnamespace, 'pg_catalog'::regnamespace) AND pronargs = 1 ORDER BY pronamespace::regnamespace::text;
-- one call site, parallel workers
SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0;
SELECT count(*) AS rows, bool_and(json_build_object('id', id, 't', t)::text = sift.json_build_object('id', id, 't', t)::text) AS same FROM jb_rows;
RESET max_parallel_workers_per_gather; RESET parallel_setup_cost; RESET parallel_tuple_cost; RESET min_parallel_table_scan_size;

-- sift before pg_catalog in the search_path takes the name for every call
SET search_path = sift, pg_catalog, public;
SELECT json_build_object('a', 1, 'b', ARRAY['x']) AS result;
SELECT to_regprocedure('json_build_object("any")') = to_regprocedure('sift.json_build_object("any")') AS takes_the_name;
RESET search_path;

DROP TABLE jb_rows;
DROP FUNCTION jb_poly(anyelement), jb_same(text), jb_run(text, text);
DROP CAST (jb_opaque AS json);
DROP FUNCTION jb_opaque_json(jb_opaque), jb_late_json(jb_late);
DROP TYPE jb_nest, jb_pair CASCADE;
DROP TYPE jb_opaque, jb_late;
DROP DOMAIN jb_posint, jb_label, jb_doc, jb_dom;
DROP TYPE jb_lvl3;
DROP EXTENSION pg_sift;
