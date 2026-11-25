-- SiftAggV6 over an index of format major 2 beyond one table of plain columns: the shredded scalars of a jsonb column (doc->>'k' and (doc->>'k')::float8), star joins of the fact
-- table with inner and left dimensions (unique or not, up to four, and a dimension that does not fit work_mem in one pass), and LATERAL unnest of an array column.
-- Every query is run with the node and without it (sift.enable_agg) and the two answers are compared; the first word says they agree, the second that the plan is the node's.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.test_create_format1 = off;
SET sift.agg_force = on;
SET sift.simd = scalar;

CREATE TABLE sf (id int PRIMARY KEY, k int, g int, v int, x float8, doc jsonb, tags text[], nums int4[]) WITH (autovacuum_enabled = false);
INSERT INTO sf SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE 1 + i % 25 END, i % 7, i % 90, (i % 40) * 0.5,
	CASE i % 12
		WHEN 0 THEN NULL
		WHEN 1 THEN '{"a": 1.5, "t": "x", "o": {"b": "p"}}'
		WHEN 2 THEN '{"a": "5", "t": 7, "o": {"b": "q"}}'
		WHEN 3 THEN '{"t": "y"}'
		WHEN 4 THEN '{"a": null, "t": null, "o": {"b": null}}'
		WHEN 5 THEN '{"a": 2.5, "t": true, "o": {"b": 3}}'
		WHEN 6 THEN '{"a": 3, "t": {"z": 1}, "o": []}'
		WHEN 7 THEN '[1, 2]'
		WHEN 8 THEN '"scalar"'
		WHEN 9 THEN jsonb_build_object('a', i * 0.25, 't', 'k' || i % 5, 'o', jsonb_build_object('b', 'r' || i % 3))
		ELSE jsonb_build_object('a', i, 't', 'k' || i % 3)
	END,
	CASE i % 6 WHEN 0 THEN NULL WHEN 1 THEN '{}' WHEN 2 THEN ARRAY['a', NULL, 'b'] WHEN 3 THEN ARRAY['t' || i % 4] ELSE ARRAY['u' || i % 3, 'u' || i % 5, 'v'] END,
	CASE i % 5 WHEN 0 THEN NULL WHEN 1 THEN '{}' ELSE ARRAY[i % 9, NULL, i % 4] END
FROM generate_series(1, 3000) i;
CREATE INDEX sf_col ON sf USING sift_col (id, k, g, v, x, doc, tags, nums) WITH (key_order = id, paths = 'doc.a float8, doc.t text, tags[] text, nums[] int4');

CREATE TABLE d1 (k int PRIMARY KEY, name text, w int);
INSERT INTO d1 SELECT i, CASE WHEN i % 8 = 0 THEN NULL ELSE 'n' || i % 5 END, i % 4 FROM generate_series(1, 20) i;
CREATE TABLE d2 (g int, tag text, z int);
INSERT INTO d2 SELECT i % 7, 't' || i % 3, i FROM generate_series(0, 20) i;
CREATE TABLE d3 (g int PRIMARY KEY, cat text);
INSERT INTO d3 SELECT i, 'c' || i % 4 FROM generate_series(0, 5) i;
CREATE TABLE d4 (v int, band text);
INSERT INTO d4 SELECT i, CASE WHEN i < 30 THEN 'low' WHEN i < 60 THEN 'mid' ELSE 'high' END FROM generate_series(0, 89) i;
ANALYZE sf;
ANALYZE d1;
ANALYZE d2;
ANALYZE d3;
ANALYZE d4;

CREATE FUNCTION answer(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO r;
	RETURN r;
END $$;

CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; plan text := ''; l text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l; END LOOP;
	a := answer(q);
	SET LOCAL sift.enable_agg = off;
	b := answer(q);
	RETURN (CASE WHEN a IS DISTINCT FROM b THEN 'DIFFERENT' ELSE 'same' END) || ' ' || (CASE WHEN plan LIKE '%SiftAggV6%' THEN 'node' ELSE 'normal' END) || ' ' || q;
END $$;

CREATE FUNCTION passes(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'Star Passes' THEN RETURN regexp_replace(l, '\D', '', 'g')::int; END IF;
	END LOOP;
	RETURN 1;
END $$;

-- Q-C01. The plans: a star join reads its dimensions whole, an unnest reads the elements of the array column
EXPLAIN (COSTS OFF) SELECT d1.name, count(*), sum(sf.v) FROM sf JOIN d1 ON sf.k = d1.k GROUP BY d1.name;
EXPLAIN (COSTS OFF) SELECT d1.name, d3.cat, count(*) FROM sf JOIN d1 ON sf.k = d1.k LEFT JOIN d3 ON sf.g = d3.g GROUP BY 1, 2;
EXPLAIN (COSTS OFF) SELECT u.e, count(*) FROM sf CROSS JOIN LATERAL unnest(sf.tags) AS u(e) GROUP BY u.e;
EXPLAIN (COSTS OFF) SELECT sf.doc->>'t', sum((sf.doc->>'a')::float8) FROM sf GROUP BY 1;
EXPLAIN (COSTS OFF) SELECT d3.cat, sf.doc->>'t', sum((sf.doc->>'a')::float8) FROM sf LEFT JOIN d3 ON sf.g = d3.g GROUP BY 1, 2;

-- Q-C02. Inner and left star joins: NULL keys, keys with no dimension row, dimension rows with a NULL payload, an ON clause that only the dimension reads
SELECT same(q) FROM unnest(ARRAY[
	'SELECT d1.name, count(*), sum(sf.v), sum(d1.w) FROM sf JOIN d1 ON sf.k = d1.k GROUP BY d1.name',
	'SELECT d1.name, count(*), sum(sf.v), sum(d1.w) FROM sf LEFT JOIN d1 ON sf.k = d1.k GROUP BY d1.name',
	'SELECT d1.name, count(*) FROM sf LEFT JOIN d1 ON sf.k = d1.k AND d1.w = 1 GROUP BY d1.name',
	'SELECT sf.g, d1.w, count(*), min(sf.x), max(sf.x) FROM sf JOIN d1 ON sf.k = d1.k WHERE sf.v > 20 GROUP BY 1, 2',
	'SELECT count(*), sum(d1.w) FROM sf JOIN d1 ON sf.k = d1.k',
	'SELECT count(*), count(d1.name) FROM sf LEFT JOIN d1 ON sf.k = d1.k WHERE sf.v < 10'
]) q;

-- Q-C03. A dimension with a key more than once expands the rows; two to four dimensions go through one batch of pairs after the other
SELECT same(q) FROM unnest(ARRAY[
	'SELECT d2.tag, count(*), sum(sf.v), sum(d2.z) FROM sf JOIN d2 ON sf.g = d2.g GROUP BY d2.tag',
	'SELECT d2.tag, count(*) FROM sf LEFT JOIN d2 ON sf.g = d2.g AND d2.z > 5 GROUP BY d2.tag',
	'SELECT d1.name, d3.cat, count(*), sum(sf.v) FROM sf JOIN d1 ON sf.k = d1.k LEFT JOIN d3 ON sf.g = d3.g GROUP BY 1, 2',
	'SELECT d1.name, d2.tag, d3.cat, count(*), sum(sf.v) FROM sf JOIN d1 ON sf.k = d1.k JOIN d2 ON sf.g = d2.g LEFT JOIN d3 ON sf.g = d3.g GROUP BY 1, 2, 3',
	'SELECT d1.name, d2.tag, d3.cat, d4.band, count(*) FROM sf JOIN d1 ON sf.k = d1.k JOIN d2 ON sf.g = d2.g JOIN d3 ON sf.g = d3.g JOIN d4 ON sf.v = d4.v GROUP BY 1, 2, 3, 4'
]) q;

-- Q-C04. A dimension of more rows than the planner counted does not fit work_mem in one pass: the fact table is read once per part of its keys
CREATE TABLE dbig (id int, name text, w int) WITH (autovacuum_enabled = false);
INSERT INTO dbig SELECT i, 'b' || i % 10, i % 100 FROM generate_series(1, 50) i;
ANALYZE dbig;
INSERT INTO dbig SELECT i, 'b' || i % 10, i % 100 FROM generate_series(51, 8000) i;
SET work_mem = '64kB';
SELECT same(q) FROM unnest(ARRAY[
	'SELECT dbig.name, count(*), sum(sf.v), sum(dbig.w) FROM sf JOIN dbig ON sf.v = dbig.id AND dbig.w * 1 = dbig.w GROUP BY 1',
	'SELECT dbig.name, count(*), sum(dbig.w) FROM sf LEFT JOIN dbig ON sf.v = dbig.id AND dbig.w * 1 = dbig.w AND dbig.w > 50 GROUP BY 1'
]) q;
SELECT passes('SELECT dbig.name, count(*) FROM sf JOIN dbig ON sf.v = dbig.id AND dbig.w * 1 = dbig.w GROUP BY 1') > 1 AS read_in_parts;
RESET work_mem;

-- Q-D01. The shredded scalars of a jsonb column: missing keys, JSON nulls, keys of another type, a document that is not an object and a SQL NULL document
SELECT same(q) FROM unnest(ARRAY[
	'SELECT doc->>''t'', count(*) FROM sf GROUP BY 1',
	'SELECT g, count(doc->>''t''), count(*) FROM sf GROUP BY g',
	'SELECT g, count(*), count((doc->>''a'')::float8), sum((doc->>''a'')::float8), min((doc->>''a'')::float8), max((doc->>''a'')::float8) FROM sf GROUP BY g',
	'SELECT doc->>''t'', sum(v), avg(x), sum((doc->>''a'')::float8) FROM sf WHERE v > 10 GROUP BY 1',
	'SELECT g, doc->>''t'', count(*), sum((doc->>''a'')::float8) FROM sf WHERE v < 60 GROUP BY 1, 2'
]) q;
-- the quals over jsonb stay with the table
SELECT same('SELECT g, count(*) FROM sf WHERE doc->>''t'' = ''x'' GROUP BY g');
-- a value the leaf cannot hold is read from the table, and a text that is not a float8 is the error of the table
INSERT INTO sf VALUES (99999, 1, 1, 1, 1, '{"a": "bad"}', NULL, NULL);
SELECT g, sum((doc->>'a')::float8) FROM sf GROUP BY g;
SET sift.enable_agg = off;
SELECT g, sum((doc->>'a')::float8) FROM sf GROUP BY g;
SET sift.enable_agg = on;
SELECT same('SELECT g, count(doc->>''t''), count(*) FROM sf GROUP BY g');
DELETE FROM sf WHERE id = 99999;

-- Q-D02. A shredded scalar and a dimension together
SELECT same(q) FROM unnest(ARRAY[
	'SELECT d1.name, sf.doc->>''t'', count(*) FROM sf JOIN d1 ON sf.k = d1.k GROUP BY 1, 2',
	'SELECT d3.cat, sum((sf.doc->>''a'')::float8), count(sf.doc->>''t'') FROM sf LEFT JOIN d3 ON sf.g = d3.g GROUP BY 1',
	'SELECT d2.tag, sf.doc->>''t'', sum((sf.doc->>''a'')::float8) FROM sf JOIN d2 ON sf.g = d2.g GROUP BY 1, 2'
]) q;

-- Q-E01. unnest of an array column: NULL arrays, empty arrays, NULL elements, an array of several elements; CROSS and LEFT LATERAL
SELECT same(q) FROM unnest(ARRAY[
	'SELECT u.e, count(*), sum(sf.v) FROM sf CROSS JOIN LATERAL unnest(sf.tags) AS u(e) GROUP BY u.e',
	'SELECT u.e, count(*), sum(sf.v) FROM sf LEFT JOIN LATERAL unnest(sf.tags) AS u(e) ON true GROUP BY u.e',
	'SELECT sf.g, count(u.e), count(*) FROM sf LEFT JOIN LATERAL unnest(sf.tags) AS u(e) ON true WHERE sf.v > 20 AND sf.k < 10 GROUP BY sf.g',
	'SELECT count(*), count(u.e) FROM sf CROSS JOIN LATERAL unnest(sf.tags) AS u(e)',
	'SELECT u.e, sf.g, count(*) FROM sf CROSS JOIN LATERAL unnest(sf.tags) u(e) WHERE sf.g IN (1, 2) GROUP BY 1, 2',
	'SELECT u.n, count(*), sum(u.n) FROM sf CROSS JOIN LATERAL unnest(sf.nums) AS u(n) GROUP BY u.n',
	'SELECT u.n, count(*) FROM sf LEFT JOIN LATERAL unnest(sf.nums) AS u(n) ON true GROUP BY u.n',
	'SELECT sf.g, u.e, count(*), sum(sf.x) FROM sf CROSS JOIN LATERAL unnest(sf.tags) AS u(e) GROUP BY 1, 2'
]) q;

-- Q-F01. FILTER over the fact table's columns, shredded scalars and the columns of an unnest is the node's; a FILTER over a dimension or a jsonb qual stays with the table
SELECT same(q) FROM unnest(ARRAY[
	'SELECT g, sum((doc->>''a'')::float8) FILTER (WHERE v > 20), count(doc->>''t'') FILTER (WHERE g > 2) FROM sf GROUP BY g',
	'SELECT doc->>''t'', count(*) FILTER (WHERE v > 20), sum(v) FILTER (WHERE x > 5) FROM sf GROUP BY 1',
	'SELECT d1.name, sum(sf.v) FILTER (WHERE d1.w > 1), count(*) FILTER (WHERE sf.g = 2) FROM sf JOIN d1 ON sf.k = d1.k GROUP BY 1',
	'SELECT u.e, count(*) FILTER (WHERE sf.v > 20), sum(sf.v) FILTER (WHERE u.e = ''a'') FROM sf, unnest(sf.tags) u(e) GROUP BY 1',
	'SELECT d1.name, count(*) FILTER (WHERE sf.doc->>''t'' = ''x'') FROM sf JOIN d1 ON sf.k = d1.k GROUP BY 1'
]) q;

-- Q-M01. Rows the index has not merged yet, updated and deleted rows, then VACUUM: the answers follow the table
INSERT INTO sf SELECT 10000 + i, 1 + i % 25, i % 7, i % 90, i * 0.5,
	CASE i % 4 WHEN 0 THEN NULL WHEN 1 THEN '{"a": 2, "t": "x", "o": {"b": "p"}}'::jsonb WHEN 2 THEN '{"t": "new"}' ELSE '{"a": "7", "t": 1}' END,
	CASE i % 5 WHEN 0 THEN NULL WHEN 1 THEN '{}' WHEN 2 THEN ARRAY['n', NULL, 'm'] ELSE ARRAY['w' || i % 3, 'n'] END,
	CASE i % 3 WHEN 0 THEN NULL WHEN 1 THEN '{}' ELSE ARRAY[i % 5, NULL] END
FROM generate_series(1, 300) i;
CREATE TEMP TABLE dml_queries AS SELECT q FROM unnest(ARRAY[
	'SELECT u.e, count(*), sum(sf.v) FROM sf CROSS JOIN LATERAL unnest(sf.tags) AS u(e) GROUP BY u.e',
	'SELECT u.e, count(*), sum(sf.v) FROM sf LEFT JOIN LATERAL unnest(sf.tags) AS u(e) ON true GROUP BY u.e',
	'SELECT u.n, count(*) FROM sf LEFT JOIN LATERAL unnest(sf.nums) AS u(n) ON true GROUP BY u.n',
	'SELECT d1.name, count(*), sum(sf.v) FROM sf JOIN d1 ON sf.k = d1.k GROUP BY d1.name',
	'SELECT d1.name, d2.tag, count(*) FROM sf LEFT JOIN d1 ON sf.k = d1.k JOIN d2 ON sf.g = d2.g GROUP BY 1, 2',
	'SELECT doc->>''t'', count(*), sum(v) FROM sf GROUP BY 1'
]) q;
SELECT same(q) FROM dml_queries;
UPDATE sf SET tags = ARRAY['q' || id % 4, 'r'], nums = ARRAY[id % 3], doc = '{"a": 4, "t": "upd"}' WHERE id % 11 = 0;
DELETE FROM sf WHERE id % 13 = 0;
SELECT same(q) FROM dml_queries;
VACUUM sf;
SELECT same(q) FROM dml_queries;
INSERT INTO sf SELECT 20000 + i, 1 + i % 25, i % 7, i % 90, i, '{"t": "late", "a": 1}', ARRAY['late', 'late2'], ARRAY[i % 2] FROM generate_series(1, 50) i;
SELECT same(q) FROM dml_queries;
UPDATE d1 SET name = 'nx' || w WHERE k % 3 = 0;
DELETE FROM d2 WHERE z > 15;
SELECT same(q) FROM dml_queries;

DROP TABLE sf, d1, d2, d3, d4, dbig;
DROP FUNCTION same(text);
DROP FUNCTION answer(text);
DROP FUNCTION passes(text);
DROP EXTENSION pg_sift;
