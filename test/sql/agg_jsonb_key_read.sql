-- The value of a key read beside the test of the key (`sum((doc ->> 'k')::float8) FILTER (WHERE doc ? 'k')`) is answered by the index. A document that is no object (an array holding
-- the key as a string, a string equal to it) has the key for `?` and reads NULL for `->>`, which the leaf of the key says as a JSON null; the source reads those rows again from the heap
-- beside the rows that hold a value the leaf cannot store (a text that is no number, a container), whose error SQL raises where a row reaches the expression. Every statement runs with the
-- scan and without it and the rows (or the error) must be the same; `scan` says that the plan is the scan, `plain` that it stays the normal one.
-- The documents: objects with the key as a number, a number in a string, null, an array, an object and a boolean; objects without the key; arrays that hold the key and arrays that do not;
-- a string equal to the key and one that is not; scalars; JSON null; the empty object and array; and a SQL NULL. The tables differ in the value that cannot be stored, all of it in group 4.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
CREATE FUNCTION kr_doc(i int, bad jsonb) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE i % 20
		WHEN 0 THEN jsonb_build_object('k', (i % 13) * 0.5 - 2, 'n', i % 7 - 3, 's', 'v' || i % 4)
		WHEN 1 THEN jsonb_build_object('k', i % 9)
		WHEN 2 THEN jsonb_build_object('n', i % 5, 's', 'w')
		WHEN 3 THEN '{"k": null, "n": null, "s": null}'::jsonb
		WHEN 4 THEN '["k"]'::jsonb
		WHEN 5 THEN '["n", "k", "s", "x"]'::jsonb
		WHEN 6 THEN '["x"]'::jsonb
		WHEN 7 THEN '"k"'::jsonb
		WHEN 8 THEN '"s"'::jsonb
		WHEN 9 THEN '"x"'::jsonb
		WHEN 10 THEN to_jsonb(i)
		WHEN 11 THEN 'true'::jsonb
		WHEN 12 THEN 'null'::jsonb
		WHEN 13 THEN jsonb_build_object('k', (i % 5)::text, 'n', (i % 3)::text, 's', '')
		WHEN 14 THEN CASE WHEN bad IS NULL THEN jsonb_build_object('k', i % 3 + 0.5) ELSE jsonb_build_object('k', bad, 'n', bad, 's', bad) END
		WHEN 15 THEN '{}'::jsonb
		WHEN 16 THEN '[]'::jsonb
		WHEN 17 THEN jsonb_build_object('k', 12345678901234567890123.5, 'n', 2000000000 + i % 3, 's', i)
		WHEN 18 THEN jsonb_build_object('k', i, 'j', i)
		ELSE jsonb_build_object('k', i % 4 - 1.5, 'n', i % 2, 's', 'x' || i % 3)
	END
$$;

-- the rows with a value of the bad kind are those of group 4 (i % 20 = 14), a SQL NULL document is one in 41
CREATE FUNCTION kr_make(tbl text, bad jsonb) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TABLE %I (id int, g int, doc jsonb) WITH (autovacuum_enabled = false)', tbl);
	EXECUTE format('INSERT INTO %I SELECT i, i %% 5, CASE WHEN i %% 41 = 0 THEN NULL ELSE kr_doc(i, %L) END FROM generate_series(1, 800) i', tbl, bad);
	EXECUTE format('CREATE INDEX %I ON %I USING sift_col (id, g, doc) WITH (paths = ''doc.k float8, doc.n int4, doc.s text'')', tbl || '_col', tbl);
END $$;
SELECT kr_make('kr', NULL);
SELECT kr_make('kb_text', '"abc"');
SELECT kr_make('kb_bool', 'true');
SELECT kr_make('kb_array', '[1, 2]');
SELECT kr_make('kb_object', '{"a": 1}');
SELECT kr_make('kb_range', '3000000000');
-- the segments of an older library, which do not flag the documents that are a string or an array: every NULL slot is read again
SET sift.test_no_roots = on;
SELECT kr_make('ko', NULL);
SELECT kr_make('ko_text', '"abc"');
RESET sift.test_no_roots;
VACUUM ANALYZE kr, kb_text, kb_bool, kb_array, kb_object, kb_range, ko, ko_text;

CREATE TABLE shapes (n int, label text, q text);
INSERT INTO shapes VALUES
  (1,  'filter sum',          $q$SELECT sum((doc ->> 'k')::float8) FILTER (WHERE doc ? 'k'), count(*) FILTER (WHERE doc ? 'k') FROM @$q$),
  (2,  'grouped',             $q$SELECT g, sum((doc ->> 'k')::float8) FILTER (WHERE doc ? 'k') FROM @ GROUP BY g$q$),
  (3,  'filter drops group 4', $q$SELECT sum((doc ->> 'k')::float8) FILTER (WHERE doc ? 'k' AND g <> 4), count(*) FILTER (WHERE doc ? 'k' AND g <> 4) FROM @$q$),
  (4,  'case else 0',         $q$SELECT sum(CASE WHEN doc ? 'k' THEN (doc ->> 'k')::float8 ELSE 0 END) FROM @$q$),
  (5,  'case avg else 0',     $q$SELECT g, avg(CASE WHEN doc ? 'n' THEN (doc ->> 'n')::int ELSE 0 END) FROM @ GROUP BY g$q$),
  (6,  'where',               $q$SELECT count(*), sum((doc ->> 'k')::float8) FROM @ WHERE doc ? 'k'$q$),
  (7,  'where or',            $q$SELECT sum((doc ->> 'k')::float8), count(*) FROM @ WHERE doc ? 'k' OR g = 0$q$),
  (8,  'where or two keys',   $q$SELECT sum((doc ->> 'k')::float8) AS sk, sum((doc ->> 'n')::int) AS sn FROM @ WHERE doc ? 'k' OR doc ? 'n'$q$),
  (9,  'any',                 $q$SELECT sum((doc ->> 'k')::float8) FILTER (WHERE doc ?| array['k', 'n']) AS sk, sum((doc ->> 'n')::int) FILTER (WHERE doc ?| array['k', 'n']) AS sn FROM @$q$),
  (10, 'all',                 $q$SELECT sum((doc ->> 'k')::float8) FILTER (WHERE doc ?& array['k', 'n']), count(*) FILTER (WHERE doc ?& array['k', 'n']) FROM @$q$),
  (11, 'not true',            $q$SELECT sum((doc ->> 'k')::float8) FILTER (WHERE (doc ? 'k') IS NOT TRUE), count(*) FILTER (WHERE (doc ? 'k') IS NOT TRUE) FROM @$q$),
  (12, 'count of the read',   $q$SELECT count((doc ->> 'k')::float8) FILTER (WHERE doc ? 'k') AS ck, count(*) FILTER (WHERE doc ? 'k') AS c FROM @$q$),
  (13, 'read without filter', $q$SELECT sum((doc ->> 'k')::float8), count(*) FILTER (WHERE doc ? 'k') FROM @$q$),
  (14, 'group by the read',   $q$SELECT (doc ->> 'k')::float8 AS kk, count(*) FILTER (WHERE doc ? 'k') FROM @ GROUP BY 1$q$),
  (15, 'compare in where',    $q$SELECT count(*) FILTER (WHERE doc ? 'k'), sum(id) FROM @ WHERE (doc ->> 'k')::float8 > 1$q$),
  (16, 'compare in filter',   $q$SELECT count(*) FILTER (WHERE doc ? 'k' AND (doc ->> 'k')::float8 > 1) FROM @$q$),
  (17, 'int key',             $q$SELECT sum((doc ->> 'n')::int) FILTER (WHERE doc ? 'n'), count(*) FILTER (WHERE doc ? 'n') FROM @$q$),
  (18, 'text key',            $q$SELECT count(doc ->> 's') FILTER (WHERE doc ? 's'), min(doc ->> 's') FILTER (WHERE doc ? 's'), max(doc ->> 's') FROM @$q$),
  (19, 'having',              $q$SELECT g, sum((doc ->> 'k')::float8) FILTER (WHERE doc ? 'k') FROM @ GROUP BY g HAVING count(*) FILTER (WHERE doc ? 'k') > 3$q$),
  (20, 'type test too',       $q$SELECT count(*) FILTER (WHERE doc ? 'k' OR jsonb_typeof(doc -> 'k') = 'number'), sum((doc ->> 'k')::float8) FILTER (WHERE doc ? 'k') FROM @$q$),
  (21, 'two reads',           $q$SELECT sum((doc ->> 'k')::float8) FILTER (WHERE doc ? 'n') AS sk, sum((doc ->> 'n')::int) FILTER (WHERE doc ? 'k') AS sn FROM @$q$),
  (22, 'document read',       $q$SELECT max(doc::text) FILTER (WHERE doc ? 'k'), sum((doc ->> 'k')::float8) FILTER (WHERE doc ? 'k') FROM @$q$),
  (23, 'path below the key',  $q$SELECT sum((doc -> 'k' ->> 'a')::float8) FILTER (WHERE doc ? 'k') FROM @$q$),
  (24, 'type guard of read',  $q$SELECT sum(CASE WHEN jsonb_typeof(doc -> 'k') = 'number' THEN (doc ->> 'k')::float8 ELSE 0 END), count(*) FILTER (WHERE doc ? 'k') FROM @$q$),
  (25, 'undeclared key',      $q$SELECT sum((doc ->> 'k')::float8) FILTER (WHERE doc ? 'zz') FROM @$q$),
  (27, 'good rows grouped',   $q$SELECT g, sum((doc ->> 'k')::float8) FILTER (WHERE doc ? 'k') AS sk, count(*) FILTER (WHERE doc ?| array['k', 'n']) AS c FROM @ WHERE g <> 4 GROUP BY g$q$),
  (28, 'good rows case',      $q$SELECT sum(CASE WHEN doc ? 'n' THEN (doc ->> 'n')::int ELSE 0 END) AS s, max(CASE WHEN doc ? 'k' THEN (doc ->> 'n')::int ELSE 0 END) AS m FROM @ WHERE g <> 4$q$),
  (29, 'good rows or',        $q$SELECT sum((doc ->> 'k')::float8) AS sk, sum((doc ->> 'n')::int) AS sn FROM @ WHERE g <> 4 AND (doc ? 'k' OR doc ? 'n')$q$),
  (30, 'good rows text key',  $q$SELECT count(doc ->> 's') FILTER (WHERE doc ? 's') AS c, min(doc ->> 's') FILTER (WHERE doc ? 's') AS lo, max(doc ->> 's') AS hi FROM @ WHERE g <> 4$q$);
-- the cast of a jsonb value raises for a JSON null before PostgreSQL 18 (the documents of group 3 hold one): the plan and the message are not printed
INSERT INTO shapes VALUES
  (31, 'jsonb cast',          $q$SELECT sum((doc -> 'k')::float8) FILTER (WHERE doc ? 'k'), count(*) FILTER (WHERE doc ? 'k') FROM @$q$),
  (32, 'jsonb cast good rows', $q$SELECT sum((doc -> 'k')::float8) FILTER (WHERE doc ? 'k' AND g <> 3) AS s, count(*) FILTER (WHERE doc ? 'k') AS c FROM @ WHERE g <> 3$q$);
-- a comparison of the read in an AND with the key test of its leaf (in any order), a jsonb_typeof guard around the read of the path it tests (a CASE or a FILTER)
INSERT INTO shapes VALUES
  (33, 'compare and key test',      $q$SELECT count(*) FILTER (WHERE doc ? 'k' AND (doc ->> 'k')::float8 > 1) AS c, sum(id) FILTER (WHERE (doc ->> 'k')::float8 <= 1 AND doc ? 'k') AS s FROM @$q$),
  (34, 'compare and other key',     $q$SELECT count(*) FILTER (WHERE doc ? 'n' AND (doc ->> 'k')::float8 > 1) FROM @$q$),
  (35, 'IN list and key test',      $q$SELECT g, count(*) FILTER (WHERE doc ? 'n' AND (doc ->> 'n')::int IN (0, 1)) AS c FROM @ GROUP BY g$q$),
  (36, 'compare, key test and null', $q$SELECT count(*) FILTER (WHERE doc ? 'k' AND (doc ->> 'k')::float8 IS NULL) AS c, count(*) FILTER (WHERE (doc ->> 'n')::int > 0 AND doc ? 'n' AND g <> 4) AS d FROM @$q$),
  (37, 'guard in a filter',         $q$SELECT sum((doc ->> 'k')::float8) FILTER (WHERE jsonb_typeof(doc -> 'k') = 'number') AS s, count(*) FILTER (WHERE jsonb_typeof(doc -> 'k') = 'number') AS c FROM @$q$),
  (38, 'guard and jsonb cast',      $q$SELECT sum((doc -> 'k')::float8) FILTER (WHERE jsonb_typeof(doc -> 'k') = 'number') AS s FROM @$q$),
  (39, 'guard of a text key',       $q$SELECT max(doc ->> 's') FILTER (WHERE jsonb_typeof(doc -> 's') = 'string') AS m, count(doc ->> 's') FILTER (WHERE jsonb_typeof(doc -> 's') = 'string') AS c FROM @$q$),
  (40, 'guard of an int key',       $q$SELECT g, sum((doc ->> 'n')::int) FILTER (WHERE jsonb_typeof(doc -> 'n') = 'number') AS s FROM @ GROUP BY g$q$),
  (41, 'guard keeps strings',       $q$SELECT sum((doc ->> 'k')::float8) FILTER (WHERE jsonb_typeof(doc -> 'k') IN ('number', 'string')) AS s FROM @$q$),
  (42, 'guard in a case, grouped',  $q$SELECT g, sum(CASE WHEN jsonb_typeof(doc -> 'n') = 'number' THEN (doc ->> 'n')::int ELSE 0 END) AS s, avg(CASE WHEN jsonb_typeof(doc -> 'k') = 'number' THEN (doc ->> 'k')::float8 ELSE 0 END) AS a FROM @ GROUP BY g$q$),
  (43, 'guard and key test',        $q$SELECT count(*) FILTER (WHERE doc ? 'k') AS c, sum((doc ->> 'k')::float8) FILTER (WHERE jsonb_typeof(doc -> 'k') = 'number') AS s FROM @$q$),
  (44, 'guard of the other key',    $q$SELECT sum((doc ->> 'k')::float8) FILTER (WHERE jsonb_typeof(doc -> 'n') = 'number') AS s FROM @$q$);

CREATE TEMP TABLE tbls AS SELECT * FROM unnest(array['kr', 'kb_text', 'kb_bool', 'kb_array', 'kb_object', 'kb_range', 'ko', 'ko_text']) WITH ORDINALITY AS t(tbl, ord);
SELECT tbl, n, label, CASE WHEN n NOT IN (31, 32) THEN res WHEN res LIKE 'error:%' THEN 'error' ELSE regexp_replace(res, '^(scan|plain) ', '') END AS outcome
FROM tbls, shapes, LATERAL (SELECT sift_helpers.agg_check_outcome(replace(q, '@', tbl)) AS res) x ORDER BY ord, n;

-- the same after writes (new documents of every shape, an update that changes the shape, a delete), and after the vacuum that rewrites the segments
INSERT INTO kr SELECT 1000 + i, i % 5, kr_doc(i + 7, NULL) FROM generate_series(1, 200) i;
INSERT INTO kb_text SELECT 1000 + i, i % 5, kr_doc(i + 7, '"abc"') FROM generate_series(1, 200) i;
INSERT INTO ko SELECT 1000 + i, i % 5, kr_doc(i + 7, NULL) FROM generate_series(1, 200) i;
UPDATE kr SET doc = kr_doc(id + 1, NULL) WHERE id % 7 = 0;
UPDATE kb_text SET doc = kr_doc(id + 1, '"abc"') WHERE id % 7 = 0;
UPDATE ko SET doc = kr_doc(id + 1, NULL) WHERE id % 7 = 0;
DELETE FROM kr WHERE id % 11 = 0;
DELETE FROM kb_text WHERE id % 11 = 0;
DELETE FROM ko WHERE id % 11 = 0;
SELECT 'after writes' AS stage, tbl, count(*) AS shapes, count(*) FILTER (WHERE res LIKE '%DIFF%') AS different, count(*) FILTER (WHERE res LIKE 'scan%' AND n NOT IN (31, 32)) AS scans
FROM (VALUES ('kr'), ('kb_text'), ('ko')) t(tbl), shapes, LATERAL (SELECT sift_helpers.agg_check_outcome(replace(q, '@', tbl)) AS res) x GROUP BY tbl ORDER BY tbl;
VACUUM kr, kb_text, ko;
SELECT 'after vacuum' AS stage, tbl, count(*) AS shapes, count(*) FILTER (WHERE res LIKE '%DIFF%') AS different, count(*) FILTER (WHERE res LIKE 'scan%' AND n NOT IN (31, 32)) AS scans
FROM (VALUES ('kr'), ('kb_text'), ('ko')) t(tbl), shapes, LATERAL (SELECT sift_helpers.agg_check_outcome(replace(q, '@', tbl)) AS res) x GROUP BY tbl ORDER BY tbl;

-- two workers (the rows read again are the leader's and the workers' alike), on a table whose rows are good and one whose group 4 holds the text that is no number
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SELECT kr_make('kp', NULL);
SELECT kr_make('kq', '"abc"');
VACUUM ANALYZE kp, kq;
SELECT sift_helpers.plan_mode_two_workers($$SELECT sum((doc ->> 'k')::float8) FILTER (WHERE doc ? 'k'), count(*) FILTER (WHERE doc ? 'k') FROM kp$$) AS plan;
SET max_parallel_workers_per_gather = 2;
SELECT 'two workers' AS stage, tbl, count(*) AS shapes, count(*) FILTER (WHERE res LIKE '%DIFF%') AS different, count(*) FILTER (WHERE res LIKE 'scan%' AND n NOT IN (31, 32)) AS scans
FROM (VALUES ('kp'), ('kq')) t(tbl), shapes, LATERAL (SELECT sift_helpers.agg_check_outcome(replace(q, '@', tbl)) AS res) x GROUP BY tbl ORDER BY tbl;

DROP TABLE shapes, tbls, kr, kb_text, kb_bool, kb_array, kb_object, kb_range, ko, ko_text, kp, kq;
DROP FUNCTION kr_make(text, jsonb), kr_doc(int, jsonb);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
