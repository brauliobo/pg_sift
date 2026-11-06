-- NOT of an existential test (containment, key existence, a path, an array overlap or inclusion) of a column with a NOT NULL constraint: the rows of the scan
-- are the rows the normal plan gives. The negation flipped a test it did not know (the equality of a text leaf is a lookup table whose operator no flag
-- reverses) and returned the rows that match: on 6000 documents `NOT (doc @> '{"b": "x"}')` gave the 2668 rows that match where the normal plan gives the 3332 that do not. A column that may be
-- NULL keeps the normal plan for the jsonb tests (the index cannot tell a NULL document from one without the member).
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
CREATE TABLE jy AS SELECT id, g, CASE WHEN id % 9 = 0 THEN NULL ELSE doc END AS doc, tags FROM jx;
CREATE INDEX jx_col ON jx USING sift_col (id, g, doc, tags) WITH (key_cut = g, paths = 'doc.b astext, doc.n int4, doc.o.p astext, doc.t[] text(8)');
CREATE INDEX jy_col ON jy USING sift_col (id, g, doc, tags) WITH (key_cut = g, paths = 'doc.b astext, doc.n int4, doc.o.p astext, doc.t[] text(8)');
VACUUM ANALYZE jx;
VACUUM ANALYZE jy;

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
  ('array equal',         $q$tags = ARRAY[3]$q$);

CREATE TABLE forms (ord serial, name text, tmpl text);
INSERT INTO forms (name, tmpl) VALUES
  ('not',          'SELECT count(*), sum(id) FROM %1$s WHERE NOT (%2$s)'),
  ('not and',      'SELECT g, count(*) FROM %1$s WHERE NOT (%2$s) AND g < 5 GROUP BY g'),
  ('filter not',   'SELECT g, count(*) FILTER (WHERE NOT (%2$s)), sum(id) FILTER (WHERE NOT (%2$s)) FROM %1$s GROUP BY g');

-- the headline: 2668 documents have b = x, so 3332 do not
SELECT count(*) FROM jx WHERE NOT (doc @> '{"b": "x"}');
CREATE TABLE qs AS SELECT row_number() OVER (ORDER BY tb.ord, t.ord, f.ord) AS n, tb.name || ': ' || t.name || ', ' || f.name AS label, format(f.tmpl, tb.name, t.cond) AS q
FROM tests t, forms f, (VALUES (1, 'jx'), (2, 'jy')) tb(ord, name);
-- jx: the column has a NOT NULL constraint; jy: it may be NULL
SELECT n, label, sift_helpers.agg_check_outcome(q) FROM qs ORDER BY n;

DROP TABLE qs, tests, forms, jx, jy;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
