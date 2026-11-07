-- NOT of an existential test (containment, key existence, a path, an array overlap or inclusion) of a column with a NOT NULL constraint: the rows of the scan
-- are the rows the normal plan gives. The negation flipped a test it did not know (the equality of a text leaf is a lookup table whose operator no flag
-- reverses) and returned the rows that match: on 6000 documents `NOT (doc @> '{"b": "x"}')` gave the 2668 rows that match where the normal plan gives the 3332 that do not. A column that may be
-- NULL is read from the presence leaf (true, NULL where the document is SQL NULL): a NULL document, '{}' and a document of undeclared keys are
-- three different rows. The index jo, built with sift.test_col_no_present, has the layout of a release before the leaf: the same answers from the normal plan.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
CREATE TABLE jx (id int PRIMARY KEY, g int, doc jsonb NOT NULL, tags int4[] NOT NULL);
INSERT INTO jx SELECT x, x % 7, CASE x % 9
  WHEN 0 THEN '{}'
  WHEN 1 THEN '{"b": "x", "n": 3}'
  WHEN 2 THEN '{"b": "x", "n": 3, "t": ["u", "v"]}'
  WHEN 3 THEN '{"b": "y", "t": []}'
  WHEN 4 THEN '{"b": "x", "o": {"p": "x"}, "t": ["u"]}'
  WHEN 5 THEN '{"b": "x", "o": {}}'
  WHEN 6 THEN '{"b": "z", "n": 4}'
  WHEN 7 THEN '{"b": null, "o": {"p": "y"}}'
  ELSE '{"o": {"p": "x"}, "n": 3}' END::jsonb,
  CASE x % 5 WHEN 0 THEN '{}' WHEN 1 THEN ARRAY[3] WHEN 2 THEN ARRAY[3, 4] ELSE ARRAY[x % 10, 3] END::int4[]
FROM generate_series(1, 6000) x;
CREATE TABLE jy AS SELECT id, g, CASE WHEN id % 9 = 0 AND id % 2 = 0 THEN NULL WHEN id % 9 = 3 AND id % 2 = 0 THEN '{"zz": 1}' WHEN id % 9 = 6 AND id % 4 = 0 THEN NULL ELSE doc END AS doc,
  CASE WHEN id % 13 = 0 THEN NULL ELSE tags END AS tags FROM jx;
CREATE TABLE jo AS SELECT * FROM jy;
CREATE TABLE jd AS SELECT id, g, CASE id % 6 WHEN 0 THEN NULL WHEN 1 THEN '[1, 2]' WHEN 2 THEN '"s"' WHEN 3 THEN 'null' WHEN 4 THEN '{"b": "x", "n": 3}' ELSE '{}' END::jsonb AS doc, tags FROM jx;
CREATE INDEX jx_col ON jx USING sift_col (id, g, doc, tags) WITH (key_cut = g, paths = 'doc.b astext, doc.n int4, doc.o.p astext, doc.t[] text(8)');
CREATE INDEX jy_col ON jy USING sift_col (id, g, doc, tags) WITH (key_cut = g, paths = 'doc.b astext, doc.n int4, doc.o.p astext, doc.t[] text(8)');
SET sift.test_col_no_present = on;
CREATE INDEX jo_col ON jo USING sift_col (id, g, doc, tags) WITH (key_cut = g, paths = 'doc.b astext, doc.n int4, doc.o.p astext, doc.t[] text(8)');
RESET sift.test_col_no_present;
CREATE INDEX jd_col ON jd USING sift_col (id, g, doc, tags) WITH (key_cut = g, paths = 'doc.b astext, doc.n int4, doc.o.p astext, doc.t[] text(8)');
VACUUM ANALYZE jx;
VACUUM ANALYZE jy;
VACUUM ANALYZE jo;
VACUUM ANALYZE jd;

CREATE TABLE tests (ord serial, name text, cond text);
INSERT INTO tests (name, cond) VALUES
  ('text leaf',           $q$doc @> '{"b": "x"}'$q$),
  ('integer leaf',        $q$doc @> '{"n": 3}'$q$),
  ('nested text leaf',    $q$doc @> '{"o": {"p": "x"}}'$q$),
  ('two keys'     ,         $q$doc @> '{"b": "x", "n": 3}'$q$),
  ('contained by',        $q$'{"b": "x"}'::jsonb <@ doc$q$),
  ('array element',       $q$doc @> '{"t": ["u"]}'$q$),
  ('key exists',          $q$doc ? 't'$q$),
  ('path exists',         $q$jsonb_path_exists(doc, '$.t[*] ? (@ == "u")')$q$),
  ('path exists member',  $q$doc @? '$.o.p'$q$),
  ('array overlap',       $q$tags && ARRAY[3]$q$),
  ('array contains',      $q$tags @> ARRAY[3, 4]$q$),
  ('array contained',     $q$tags <@ ARRAY[3, 4, 5]$q$),
  ('array equal',         $q$tags = ARRAY[3]$q$),
  ('both containments',   $q$doc @> '{"b": "x"}' AND doc @> '{"n": 3}'$q$),
  ('either containment',  $q$doc @> '{"b": "x"}' OR doc @> '{"n": 3}'$q$),
  ('containment or g',    $q$doc @> '{"b": "x"}' OR g = 3$q$),
  ('containment and g',   $q$doc @> '{"b": "x"}' AND g < 4$q$),
  ('key or containment',  $q$doc ? 't' OR doc @> '{"b": "x"}'$q$),
  ('array and key',       $q$tags && ARRAY[3] AND doc ? 't'$q$),
  ('document is null',    $q$doc IS NULL$q$),
  ('document not null',   $q$doc IS NOT NULL$q$);

CREATE TABLE forms (ord serial, name text, tmpl text);
INSERT INTO forms (name, tmpl) VALUES
  ('where',        'SELECT count(*), sum(id) FROM %1$s WHERE %2$s'),
  ('not',          'SELECT count(*), sum(id) FROM %1$s WHERE NOT (%2$s)'),
  ('not and',      'SELECT g, count(*) FROM %1$s WHERE NOT (%2$s) AND g < 5 GROUP BY g'),
  ('filter not',   'SELECT g, count(*) FILTER (WHERE NOT (%2$s)), sum(id) FILTER (WHERE NOT (%2$s)) FROM %1$s GROUP BY g'),
  ('is false',     'SELECT count(*), sum(id) FROM %1$s WHERE (%2$s) IS FALSE'),
  ('case else 0',  'SELECT g, sum(CASE WHEN NOT (%2$s) THEN id ELSE 0 END), count(*) FILTER (WHERE NOT (%2$s)) FROM %1$s GROUP BY g');

-- the headline: 2668 documents have b = x, so 3332 do not
SELECT count(*) FROM jx WHERE NOT (doc @> '{"b": "x"}');
CREATE TABLE qs AS SELECT row_number() OVER (ORDER BY tb.ord, t.ord, f.ord) AS n, tb.name || ': ' || t.name || ', ' || f.name AS label, format(f.tmpl, tb.name, t.cond) AS q
FROM tests t, forms f, (VALUES (1, 'jx'), (2, 'jy'), (3, 'jo'), (4, 'jd')) tb(ord, name);
-- the presence leaf: jx has a NOT NULL document and needs none, the nullable ones have it, jo has the layout of a release before it
SELECT i.indrelid::regclass AS tbl, bool_or(l.path = 'doc#present') AS has_leaf FROM pg_index i, LATERAL sift.col_leaves(i.indexrelid) l
WHERE i.indrelid IN ('jx'::regclass, 'jy'::regclass, 'jo'::regclass, 'jd'::regclass) AND i.indisready AND i.indexrelid::regclass::text LIKE 'j%\_col' GROUP BY 1 ORDER BY 1;
-- jx: the column has a NOT NULL constraint; jy: it may be NULL (NULL, {} and a document of undeclared keys among the rows); jo: jy with the index of a release before
-- the presence leaf; jd: documents that are not objects (an array, a string, a JSON null) beside the NULL ones, which the index holds as overflow
SELECT n, label, sift_helpers.agg_check_outcome(q) FROM qs ORDER BY n;

DROP TABLE qs, tests, forms, jx, jy, jo, jd;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
