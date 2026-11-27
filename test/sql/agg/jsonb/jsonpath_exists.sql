-- doc @? '$.items[*] ? (..)' and jsonb_path_exists(doc, '$.items[*] ? (..)') on an index of format major 2 that keeps the members of the elements of doc.items: SiftAggV6 tests the elements as
-- it tests those of the array doc -> 'items', and a row the leaves do not hold (a document that is no object, items that is no array, a member of another JSON type) is decided by the call itself, lax
-- mode, strict mode and errors included. A comparison with a float leaf (the leaf rounds), a filter on a scalar path and Format 1 keep the normal plan. Each statement runs with the scan and without
-- it: `scan` or `plain`, then whether the rows are the same; `why`: an arm the plan uses. NOT of the call is NULL for a NULL document only (the presence leaf of the column tells it from one
-- without items), and a strict path that is not silent raises for a document without items, so it keeps the normal plan.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
CREATE FUNCTION jp_doc(i int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE i % 10
		WHEN 0 THEN '[{"items": [{"kind": "k_1", "n": 9}]}]'::jsonb
		WHEN 1 THEN '{"items": {"kind": "k_1", "n": 9}}'::jsonb
		WHEN 2 THEN '{"items": [{"kind": "k_1", "n": "9"}, {"kind": 1, "n": 9}]}'::jsonb
		WHEN 3 THEN '{"items": "k_1"}'::jsonb
		WHEN 4 THEN '{"other": 1}'::jsonb
		WHEN 5 THEN '{"items": [{"kind": "k_1"}, {"n": 9}, {"kind": null, "n": null}]}'::jsonb
		ELSE jsonb_build_object('k', i % 50, 'items', jsonb_build_array(jsonb_build_object('kind', 'k_' || i % 3, 'n', i % 11, 'score', i % 10), jsonb_build_object('kind', 'k_' || (i + 1) % 3, 'n', i % 7)))
	END
$$;
CREATE FUNCTION jp_make(tbl text, f1 bool) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.test_create_format1', f1::text, true);
	EXECUTE format('CREATE TABLE %I (id int, g int, doc jsonb) WITH (autovacuum_enabled = false)', tbl);
	EXECUTE format('INSERT INTO %I SELECT i, i %% 4, CASE WHEN i %% 23 = 0 THEN NULL ELSE jp_doc(i) END FROM generate_series(1, 1200) i', tbl);
	EXECUTE format('CREATE INDEX %I ON %I USING sift_col (id, g, doc) WITH (paths = ''doc.k int4, doc.items[]{kind text, n int4, score float8}'')', tbl || '_col', tbl);
END $$;
SELECT jp_make('jp', false), jp_make('jp_f1', true);
VACUUM ANALYZE jp, jp_f1;
SELECT sift._test_index_format('jp_col') AS f2, sift._test_index_format('jp_f1_col') AS f1;

CREATE FUNCTION jp_check(q text) RETURNS text LANGUAGE sql AS $$
	SELECT sift_helpers.agg_check_outcome(q) || CASE WHEN EXISTS (SELECT 1 FROM sift.explain_why(q) WHERE used) THEN ' why' ELSE '' END
$$;
CREATE TABLE shapes (n int, label text, q text);
INSERT INTO shapes VALUES
  (1,  'one member',          $q$SELECT g, count(*) FROM $T WHERE doc @? '$.items[*] ? (@.kind == "k_1")' GROUP BY g$q$),
  (2,  'same element',        $q$SELECT g, count(*), sum(id) FROM $T WHERE doc @? '$.items[*] ? (@.kind == "k_1" && @.n >= 8)' GROUP BY g$q$),
  (3,  'function',            $q$SELECT count(*) FROM $T WHERE jsonb_path_exists(doc, '$.items[*] ? (@.kind == "k_2" || @.n < 2)')$q$),
  (4,  'negated filter',      $q$SELECT count(*) FROM $T WHERE doc @? '$.items[*] ? (!(@.kind == "k_1"))'$q$),
  (5,  'null member',         $q$SELECT count(*) FROM $T WHERE doc @? '$.items[*] ? (@.kind == null)'$q$),
  (6,  'not exists',          $q$SELECT count(*) FROM $T WHERE NOT (doc @? '$.items[*] ? (@.n == 9)')$q$),
  (7,  'strict',              $q$SELECT count(*) FROM $T WHERE doc @? 'strict $.items[*] ? (@.kind == "k_1")'$q$),
  (8,  'variables',           $q$SELECT count(*) FROM $T WHERE jsonb_path_exists(doc, '$.items[*] ? (@.n == $v)', '{"v": 9}')$q$),
  (9,  'beside a filter',     $q$SELECT count(*) FROM $T WHERE doc @? '$.items[*] ? (@.kind == "k_0")' AND g < 2$q$),
  (10, 'float member',        $q$SELECT count(*) FROM $T WHERE doc @? '$.items[*] ? (@.score >= 8)'$q$),
  (11, 'scalar path',         $q$SELECT count(*) FROM $T WHERE doc @? '$.k ? (@ == 42)'$q$),
  (12, 'undeclared member',   $q$SELECT count(*) FROM $T WHERE doc @? '$.items[*] ? (@.zz == 1)'$q$),
  (13, 'undeclared array',    $q$SELECT count(*) FROM $T WHERE doc @? '$.parts[*] ? (@.kind == "k_1")'$q$),
  (14, 'array of the doc',    $q$SELECT count(*) FROM $T WHERE jsonb_path_exists(doc -> 'items', '$[*] ? (@.kind == "k_1" && @.n >= 8)')$q$),
  (15, 'strict, not silent',  $q$SELECT count(*) FILTER (WHERE jsonb_path_exists(doc, 'strict $.items[*] ? (@.kind == "k_1")')) FROM $T WHERE g = 9$q$);

SELECT tbl, n, label, jp_check(replace(q, '$T', tbl)) AS outcome FROM (VALUES (1, 'jp'), (2, 'jp_f1')) t(ord, tbl), shapes ORDER BY ord, n;

-- NOT of the call where the leaves hold every document (objects only): one without items is false, so its row is counted; a NULL one is NULL
CREATE TABLE jp_nk AS SELECT i AS id, i % 4 AS g, CASE WHEN i % 23 = 0 THEN NULL WHEN i % 10 = 4 THEN '{"other": 1}'::jsonb
	ELSE jsonb_build_object('items', jsonb_build_array(jsonb_build_object('n', i % 11))) END AS doc FROM generate_series(1, 1200) i;
CREATE INDEX jp_nk_col ON jp_nk USING sift_col (id, g, doc) WITH (paths = 'doc.items[]{n int4}');
VACUUM ANALYZE jp_nk;
SELECT jp_check($q$SELECT g, count(*) FILTER (WHERE NOT (doc @? '$.items[*] ? (@.n == 9)')) FROM jp_nk GROUP BY g$q$) AS not_in_filter,
	   jp_check($q$SELECT count(*) FROM jp_nk WHERE NOT jsonb_path_exists(doc, '$.items[*] ? (@.n == 9)')$q$) AS not_in_where;

-- the error of a call the heap decides is raised as PostgreSQL raises it
SET sift.enable_agg = on;
SELECT count(*) FROM jp WHERE jsonb_path_exists(doc, 'strict $.items[*] ? (@.kind == "k_1")');
SELECT count(*) FROM jp WHERE doc @? '$.items[*] ? (@.kind == "k_1" && @.n >= 8)';

DROP TABLE shapes, jp, jp_f1, jp_nk;
DROP FUNCTION jp_check(text), jp_make(text, bool), jp_doc(int);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
