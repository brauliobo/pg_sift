-- Arrays inside arrays in the paths of a jsonb column (the sift_col `paths` option, sift.table_set_paths, sift.table_set_option(..., 'paths', ...)): the declaration is accepted, the arrays of
-- the first level are leaves of the index, the member that holds a second array is one more leaf whose arrays overflow with their exact bytes, and every query answers as the heap does.
-- `node` says the plan is the scan of the index, the second word whether the answer is the heap's with the node and the third without it.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;

CREATE TABLE nh (id int PRIMARY KEY, j jsonb) WITH (autovacuum_enabled = false);
INSERT INTO nh SELECT g, CASE WHEN g % 11 = 0 THEN NULL WHEN g % 13 = 0 THEN '{"a": []}' WHEN g % 17 = 0 THEN '{"a": [{"x": "k0", "b": [{"c": 1}]}], "top": 1}' ELSE
	jsonb_build_object('top', g % 7, 'a', jsonb_build_array(
		jsonb_build_object('x', 'k' || g % 5, 'n', g % 3, 'b', jsonb_build_array(jsonb_build_object('c', g % 4, 'd', 'd' || g % 6, 'e', jsonb_build_array(g % 3, 1)), jsonb_build_object('c', g % 2, 'd', 'e'))),
		jsonb_build_object('x', 'k' || g % 2, 'n', 1, 'b', jsonb_build_array()),
		jsonb_build_object('x', 'z', 'n', 2))) END
FROM generate_series(1, 3000) g;

-- Format 1 and Format 2.0, an index and a table of each
CREATE TABLE nc1 (id int PRIMARY KEY, j jsonb) WITH (autovacuum_enabled = false);
CREATE TABLE nc2 (id int PRIMARY KEY, j jsonb) WITH (autovacuum_enabled = false);
SET sift.test_create_format1 = on;
CREATE TABLE nt1 (id int, j jsonb) USING sift_table;
SET sift.test_create_format1 = off;
CREATE TABLE nt2 (id int, j jsonb) USING sift_table;
INSERT INTO nc1 SELECT * FROM nh;
INSERT INTO nc2 SELECT * FROM nh;
SET sift.test_create_format1 = on;
INSERT INTO nt1 SELECT * FROM nh;
SET sift.test_create_format1 = off;
INSERT INTO nt2 SELECT * FROM nh;

-- a nested array is not refused: CREATE INDEX and the declaration of a table succeed
SET sift.test_create_format1 = on;
CREATE INDEX nc1_col ON nc1 USING sift_col (id, j) WITH (paths = 'j.top int4, j.a[]{x text, n int4, b[]{c int4, d text, e[] int4}}');
SET sift.test_create_format1 = off;
CREATE INDEX nc2_col ON nc2 USING sift_col (id, j) WITH (paths = 'j.top int4, j.a[]{x text, n int4, b[]{c int4, d text, e[] int4}}');
SELECT sift.table_set_paths('nt1', 'j', 'j.top int4, j.a[]{x text, n int4, b[]{c int4, d text, e[] int4}}');
SELECT sift.table_set_paths('nt2', 'j', 'j.top int4, j.a[]{x text, n int4, b[]{c int4, d text, e[] int4}}');
SELECT sift.table_set_option('nt2', 'paths', 'j.top int4, j.a[]{x text, n int4, b[]{c int4, d text}}');

-- the leaves: the member of the nested arrays stands for them, once, whatever they declare
SELECT path FROM sift.col_leaves('nc1_col') ORDER BY leaf;
SELECT path FROM sift.col_leaves('nc2_col') ORDER BY leaf;

-- a declaration of nested paths alone has the one leaf of the member; two ways to name it, and a list in a list, hold one or none
CREATE TABLE only_nested (id int, j jsonb);
INSERT INTO only_nested SELECT id, j FROM nh WHERE id < 50;
SET sift.test_create_format1 = on;
CREATE INDEX only_nested_1 ON only_nested USING sift_col (id, j) WITH (paths = 'j.a[]{b[]{c int4}}');
CREATE INDEX only_nested_2 ON only_nested USING sift_col (id, j) WITH (paths = 'j.a[]{b[]{c int4}, b[]{d text}, b[] int4}');
CREATE INDEX only_nested_3 ON only_nested USING sift_col (id, j) WITH (paths = 'j.a[]{b[]{c[]{d int4}}, x text}');
CREATE INDEX only_nested_4 ON only_nested USING sift_col (id, j) WITH (paths = 'j.m.*{b[]{c int4}}');
SELECT count(*) FROM only_nested;
DROP TABLE only_nested;

-- the other errors of a declaration stay
CREATE TABLE bad (id int, j jsonb);
CREATE INDEX bad_type ON bad USING sift_col (id, j) WITH (paths = 'j.a[]{b[]{c banana}}');
CREATE INDEX bad_col ON bad USING sift_col (id, j) WITH (paths = 'nope.a[]{b[]{c int4}}');
CREATE INDEX bad_twice ON bad USING sift_col (id, j) WITH (paths = 'j.a[]{b text, b text}');
DROP TABLE bad;

-- rows written after the declaration, a rewrite, and the checks (the setting names the format of the table a statement writes or rewrites)
INSERT INTO nh SELECT 100000 + id, j FROM nh WHERE id <= 300;
UPDATE nh SET j = jsonb_set(j, '{top}', '9') WHERE id % 10 = 3 AND j ? 'top';
DELETE FROM nh WHERE id % 19 = 0;
INSERT INTO nc1 SELECT * FROM nh WHERE id > 100000;
INSERT INTO nc2 SELECT * FROM nh WHERE id > 100000;
UPDATE nc1 SET j = jsonb_set(j, '{top}', '9') WHERE id % 10 = 3 AND j ? 'top';
UPDATE nc2 SET j = jsonb_set(j, '{top}', '9') WHERE id % 10 = 3 AND j ? 'top';
DELETE FROM nc1 WHERE id % 19 = 0;
DELETE FROM nc2 WHERE id % 19 = 0;
SET sift.test_create_format1 = on;
INSERT INTO nt1 SELECT * FROM nh WHERE id > 100000;
UPDATE nt1 SET j = jsonb_set(j, '{top}', '9') WHERE id % 10 = 3 AND j ? 'top';
DELETE FROM nt1 WHERE id % 19 = 0;
VACUUM FULL nt1;
SET sift.test_create_format1 = off;
INSERT INTO nt2 SELECT * FROM nh WHERE id > 100000;
UPDATE nt2 SET j = jsonb_set(j, '{top}', '9') WHERE id % 10 = 3 AND j ? 'top';
DELETE FROM nt2 WHERE id % 19 = 0;
VACUUM FULL nt2;
VACUUM ANALYZE nh;
VACUUM ANALYZE nc1;
VACUUM ANALYZE nc2;
VACUUM ANALYZE nt1;
VACUUM ANALYZE nt2;
SELECT count(*) AS problems FROM sift.check('nc1_col');
SELECT count(*) AS problems FROM sift.check('nc2_col');
SELECT count(*) AS problems FROM sift.check('nt1');
SELECT count(*) AS problems FROM sift.check('nt2');
SELECT (SELECT count(*) FROM nh) = (SELECT count(*) FROM nc1) AS nc1_rows, (SELECT count(*) FROM nh) = (SELECT count(*) FROM nc2) AS nc2_rows,
	(SELECT count(*) FROM nh) = (SELECT count(*) FROM nt1) AS nt1_rows, (SELECT count(*) FROM nh) = (SELECT count(*) FROM nt2) AS nt2_rows;

CREATE FUNCTION answer(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO r;
	RETURN r;
END $$;

CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE want text := answer(replace(q, '%T', 'nh')); t text; plan text; l text; out text := '';
BEGIN
	FOREACH t IN ARRAY ARRAY['nc1', 'nc2', 'nt1', 'nt2'] LOOP
		plan := '';
		SET LOCAL sift.enable_agg = on;
		FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || replace(q, '%T', t) LOOP plan := plan || l; END LOOP;
		out := out || ' ' || t || ':' || (CASE WHEN plan LIKE '%Custom Scan (SiftAgg%' THEN 'node' ELSE 'normal' END) || '/' ||
			(CASE WHEN answer(replace(q, '%T', t)) IS DISTINCT FROM want THEN 'DIFFERENT' ELSE 'same' END);
		SET LOCAL sift.enable_agg = off;
		out := out || '/' || (CASE WHEN answer(replace(q, '%T', t)) IS DISTINCT FROM want THEN 'DIFFERENT' ELSE 'same' END);
	END LOOP;
	RETURN out || '  ' || q;
END $$;

-- first level: the elements of `a` are leaves of the index
SELECT same(q) FROM unnest(ARRAY[
	$$SELECT a.x, count(*), sum(a.n) FROM %T t, jsonb_to_recordset(t.j->'a') a(x text, n int) GROUP BY 1$$,
	$$SELECT a.x, count(*) FROM %T t, jsonb_to_recordset(t.j->'a') a(x text, n int) WHERE a.n >= 1 AND t.id % 2 = 0 GROUP BY 1$$,
	$$SELECT count(*) FROM %T t WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(t.j->'a') a(x text, n int) WHERE a.x = 'k1' AND a.n = 1)$$,
	$$SELECT t.j->>'top', count(*) FROM %T t GROUP BY 1$$,
	$$SELECT count(*), sum(id) FROM %T t WHERE j->>'top' = '9'$$
]) q;

-- second level and two arrays of one row: the plan is PostgreSQL's, the answer the heap's
SELECT same(q) FROM unnest(ARRAY[
	$$SELECT b.c, count(*) FROM %T t, jsonb_to_recordset(t.j->'a') a(x text, b jsonb), jsonb_to_recordset(a.b) b(c int) GROUP BY 1$$,
	$$SELECT b.d, count(*) FROM %T t, jsonb_to_recordset(t.j->'a') a(b jsonb), jsonb_to_recordset(a.b) b(c int, d text) WHERE b.c > 1 GROUP BY 1$$,
	$$SELECT count(*) FROM %T t, jsonb_array_elements(t.j->'a') a, jsonb_array_elements(a->'b') b WHERE (b->>'c')::int = 2$$,
	$$SELECT count(*) FROM %T t, jsonb_array_elements(t.j->'a') a, jsonb_array_elements(a->'b') b, jsonb_array_elements(b->'e') e WHERE e::int = 0$$,
	$$SELECT count(*) FROM %T t WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(t.j->'a') a(x text, b jsonb) WHERE a.x = 'k0' AND jsonb_array_length(a.b) > 1)$$,
	$$SELECT count(*) FROM %T t, jsonb_to_recordset(t.j->'a') a(x text), jsonb_array_elements(t.j->'a') e$$,
	$$SELECT a.x, a.b FROM %T t, jsonb_to_recordset(t.j->'a') a(x text, b text) WHERE t.id < 20$$,
	$$SELECT count(*), sum(id) FROM %T t WHERE j @> '{"a":[{"b":[{"c":3}]}]}'$$,
	$$SELECT count(*), sum(id) FROM %T t WHERE j @? '$.a[*].b[*] ? (@.c == 1)'$$,
	$$SELECT t.id, t.j->'a'->0->'b' FROM %T t WHERE t.id IN (1, 2, 3, 100001)$$
]) q;

DROP TABLE nh, nc1, nc2, nt1, nt2;
DROP FUNCTION same(text), answer(text);
DROP EXTENSION pg_sift;
