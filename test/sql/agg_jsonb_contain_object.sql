-- `doc @> '{...}'` on an object document, as a condition of the WHERE: a member the leaf compares is a test of the leaf; a boolean on an astext leaf is its text and the JSON type
-- `boolean` (the chunks keep the kind of each value, so a string "true" does not pass); a JSON null is the type `null` (agg/plan_contains.c, contains_conjuncts). Every
-- statement runs with sift on and off and the rows are compared (md5 of the sorted rows); `engaged` says the scan ran. jd holds documents that are no object (an array, a
-- string, a number, a JSON null) and values of another type than the leaf says: the rows must still be the same. A path no leaf of the index holds is refused as
-- jsonb_path_not_declared, naming it.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
CREATE TABLE jc (id int PRIMARY KEY, g int, doc jsonb);
INSERT INTO jc SELECT x, x % 7, CASE x % 9
  WHEN 0 THEN NULL
  WHEN 1 THEN jsonb_build_object('b', 'x', 'flag', true, 'n', 3)
  WHEN 2 THEN jsonb_build_object('b', 'x', 'flag', 'true', 'n', 3)
  WHEN 3 THEN jsonb_build_object('b', 'y', 'flag', false, 'k', NULL)
  WHEN 4 THEN jsonb_build_object('b', 'x', 'flag', true, 'o', jsonb_build_object('p', true, 'q', NULL))
  WHEN 5 THEN jsonb_build_object('b', 'x', 'flag', 'false', 'o', '{}'::jsonb)
  WHEN 6 THEN jsonb_build_object('b', 'z', 'flag', 1, 'n', 4)
  WHEN 7 THEN jsonb_build_object('b', 'x', 'flag', NULL, 'k', NULL)
  ELSE jsonb_build_object('b', 'x', 'o', jsonb_build_object('p', 'true')) END
FROM generate_series(1, 6000) x;
CREATE TABLE jd AS SELECT id, g, CASE WHEN id % 5 = 0 THEN '[{"b": "x", "flag": true}]'::jsonb WHEN id % 11 = 0 THEN '"x"'::jsonb WHEN id % 13 = 0 THEN '7'::jsonb WHEN id % 17 = 0 THEN 'null'::jsonb ELSE doc END AS doc FROM jc;
CREATE INDEX jc_col ON jc USING sift_col (id, g, doc) WITH (key_cut = g, paths = 'doc.b astext, doc.flag astext, doc.n int4, doc.k astext, doc.o.p astext, doc.o.q astext');
CREATE INDEX jd_col ON jd USING sift_col (id, g, doc) WITH (key_cut = g, paths = 'doc.b astext, doc.flag astext, doc.n int4, doc.k astext, doc.o.p astext, doc.o.q astext');
VACUUM ANALYZE jc;
VACUUM ANALYZE jd;

CREATE TABLE shapes (ord serial, name text, cond text);
INSERT INTO shapes (name, cond) VALUES
  ('string',                  $q$doc @> '{"b": "x"}'$q$),
  ('boolean true',            $q$doc @> '{"flag": true}'$q$),
  ('boolean false',           $q$doc @> '{"flag": false}'$q$),
  ('string and boolean',      $q$doc @> '{"b": "x", "flag": true}'$q$),
  ('json null',               $q$doc @> '{"k": null}'$q$),
  ('json null and string',    $q$doc @> '{"k": null, "b": "x"}'$q$),
  ('nested boolean',          $q$doc @> '{"o": {"p": true}}'$q$),
  ('nested boolean string',   $q$doc @> '{"o": {"p": "true"}}'$q$),
  ('nested null',             $q$doc @> '{"o": {"q": null}, "b": "x"}'$q$),
  ('nested both',             $q$doc @> '{"o": {"p": true, "q": null}, "b": "x"}'$q$),
  ('integer member',          $q$doc @> '{"n": 3, "flag": true}'$q$),
  ('string that reads true',  $q$doc @> '{"flag": "true"}'$q$),
  ('string that reads false', $q$doc @> '{"flag": "false"}'$q$),
  ('string that reads a number', $q$doc @> '{"flag": "1"}'$q$),
  ('number in a text leaf',   $q$doc @> '{"flag": 1}'$q$),
  ('empty nested object',     $q$doc @> '{"o": {}}'$q$),
  ('contained by',            $q$'{"flag": true, "b": "x"}'::jsonb <@ doc$q$),
  ('with another condition',  $q$doc @> '{"flag": true}' AND g < 3$q$),
  ('under OR',                $q$doc @> '{"flag": true}' OR g = 3$q$),
  ('under NOT',               $q$NOT (doc @> '{"flag": true}')$q$),
  ('undeclared',              $q$doc @> '{"zz": "x"}'$q$),
  ('undeclared with boolean', $q$doc @> '{"flag": true, "zz": 1}'$q$);

CREATE FUNCTION shape_check(q text) RETURNS TABLE (engaged bool, same bool) LANGUAGE plpgsql AS
$$ DECLARE l text; plan text := ''; a text; b text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  engaged := plan LIKE '%Custom Scan (SiftAgg)%';
  PERFORM set_config('sift.enable_agg', 'on', true);
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO a;
  PERFORM set_config('sift.enable_agg', 'off', true);
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO b;
  PERFORM set_config('sift.enable_agg', 'on', true);
  same := a IS NOT DISTINCT FROM b;
  RETURN NEXT;
END $$;
SELECT s.name, c.* FROM shapes s, LATERAL shape_check('SELECT g, count(*), sum(id) FROM jc WHERE ' || s.cond || ' GROUP BY g') c ORDER BY s.ord;
SELECT s.name, c.* FROM shapes s, LATERAL shape_check('SELECT count(*) FROM jc WHERE ' || s.cond) c ORDER BY s.ord;
-- documents that are no object, and values of another type than the leaf says
SELECT s.name, c.* FROM shapes s, LATERAL shape_check('SELECT g, count(*), sum(id) FROM jd WHERE ' || s.cond || ' GROUP BY g') c ORDER BY s.ord;

SELECT count(*) FROM jc WHERE doc @> '{"b": "x", "flag": true}';
SELECT count(*) FROM jc WHERE doc @> '{"flag": "true"}';
SELECT count(*) FROM jc WHERE doc @> '{"o": {"p": true, "q": null}, "b": "x"}';

-- the refusal names the path no leaf holds
SET client_min_messages = notice;
SET sift.explain_refusals = on;
EXPLAIN (COSTS OFF) SELECT count(*) FROM jc WHERE doc @> '{"zz": "x"}';
EXPLAIN (COSTS OFF) SELECT count(*) FROM jc WHERE doc @> '{"flag": true, "o": {"zz": 1}}';
EXPLAIN (COSTS OFF) SELECT count(*) FROM jc WHERE doc ? 'zz';
EXPLAIN (COSTS OFF) SELECT count(*) FROM jc WHERE doc ?| array['b', 'zz'];
RESET sift.explain_refusals;

DROP FUNCTION shape_check(text);
DROP TABLE shapes, jc, jd;
DROP EXTENSION pg_sift;
