-- `doc @> '{...}'` and `'{...}' <@ doc` of a jsonb column in the forms that are no plain conjunct of the WHERE: NOT, IS NOT TRUE, IS FALSE, CASE (an aggregate
-- of CASE WHEN c THEN x ELSE y END is a FILTER on c and one on `c IS NOT TRUE`), FILTER, OR and AND with another condition, a key. Every statement runs with sift
-- on and off and the rows are compared (md5 of the sorted rows); `engaged` says the scan ran. jw holds a NULL document, a JSON null, empty and nested objects,
-- numbers in different spellings, arrays of scalars, unicode keys, a number as the document, two long documents, and in jd the values a leaf cannot store (overflow rows); jn is the same with a NOT NULL document. A NULL document makes the containment NULL: NOT of it keeps no row, IS NOT TRUE of it
-- keeps the row: NOT and IS FALSE on the nullable column read the leaf of option document_present (true, NULL for a NULL document), which the index of a release
-- before the option does not have (agg_jsonb_not_exist keeps that index on the normal plan).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
CREATE TABLE jw (id int PRIMARY KEY, g int, doc jsonb);
INSERT INTO jw SELECT x, x % 7, CASE x % 26
  WHEN 0 THEN NULL
  WHEN 1 THEN 'null'
  WHEN 2 THEN '{}'
  WHEN 3 THEN '{"b": true}'
  WHEN 4 THEN '{"b": "x"}'
  WHEN 5 THEN '{"b": "y", "n": 3}'
  WHEN 6 THEN '{"b": 1}'
  WHEN 7 THEN '{"b": "1"}'
  WHEN 8 THEN '{"b": 1.0, "n": 3.0}'
  WHEN 9 THEN '{"b": null, "n": 3}'
  WHEN 10 THEN '{"n": 4}'
  WHEN 11 THEN '{"n": 1e2, "b": "x"}'
  WHEN 12 THEN '{"o": {"p": "x"}}'
  WHEN 13 THEN '{"o": {}, "b": "x"}'
  WHEN 14 THEN '{"o": "x"}'
  WHEN 15 THEN '{"t": ["u", "v"], "b": "x"}'
  WHEN 16 THEN '{"t": [], "b": "x"}'
  WHEN 17 THEN '{"t": ["w", "u", "w"]}'
  WHEN 18 THEN '{"ключ": "значение", "b": "é", "s": "é"}'
  WHEN 19 THEN jsonb_build_object('b', 'x', 'pad', repeat('p', 9000), 's', 'x')
  WHEN 20 THEN jsonb_build_object('b', 'x', 'pad', repeat('q', 20000), 'o', jsonb_build_object('p', 'x'))
  WHEN 21 THEN '{"b": false}'
  WHEN 22 THEN '{"b": 2}'
  WHEN 23 THEN '7'
  WHEN 24 THEN '{"s": "x", "t": ["v"], "n": 3}'
  ELSE '{"b": "x", "s": "x", "o": {"p": "x"}, "t": ["u"], "n": 3}' END::jsonb
FROM generate_series(1, 12000) x;
CREATE TABLE jn AS SELECT id, g, coalesce(doc, '{}') AS doc FROM jw;
ALTER TABLE jn ALTER doc SET NOT NULL;
CREATE TABLE jd AS SELECT id, g, CASE id % 26 WHEN 3 THEN '[]' WHEN 10 THEN '{"n": 3.5}' WHEN 17 THEN '{"t": "u"}' WHEN 20 THEN jsonb_build_object('b', 'x', 's', repeat('long', 40)) WHEN 21 THEN '["x"]' WHEN 22 THEN '"x"' ELSE doc END::jsonb AS doc FROM jw;
CREATE INDEX jw_col ON jw USING sift_col (id, g, doc) WITH (key_cut = g, paths = 'doc.b astext, doc.n int4, doc.s text(8), doc.o.p astext, doc.t[] text(8)');
CREATE INDEX jd_col ON jd USING sift_col (id, g, doc) WITH (key_cut = g, paths = 'doc.b astext, doc.n int4, doc.s text(8), doc.o.p astext, doc.t[] text(8)');
CREATE INDEX jn_col ON jn USING sift_col (id, g, doc) WITH (key_cut = g, paths = 'doc.b astext, doc.n int4, doc.s text(8), doc.o.p astext, doc.t[] text(8)');
VACUUM ANALYZE jw;
VACUUM ANALYZE jn;
VACUUM ANALYZE jd;

CREATE TABLE tests (ord serial, name text, cond text);
INSERT INTO tests (name, cond) VALUES
  ('string',            $q$doc @> '{"b": "x"}'$q$),
  ('string and integer', $q$doc @> '{"b": "x", "n": 3}'$q$),
  ('number member',     $q$doc @> '{"b": 1}'$q$),
  ('nested',            $q$doc @> '{"o": {"p": "x"}}'$q$),
  ('short text leaf',   $q$doc @> '{"s": "x"}'$q$),
  ('array element',     $q$doc @> '{"t": ["u"]}'$q$),
  ('unicode',           $q$doc @> '{"b": "é"}'$q$),
  ('contained by',      $q$'{"b": "x"}'::jsonb <@ doc$q$),
  ('decimal spelling',  $q$doc @> '{"n": 3.0}'$q$),
  ('boolean member',    $q$doc @> '{"b": true}'$q$),
  ('json null',         $q$doc @> '{"b": null}'$q$);

CREATE TABLE forms (ord serial, name text, tmpl text);
INSERT INTO forms (name, tmpl) VALUES
  ('where',                  'SELECT count(*), sum(id) FROM %1$s WHERE %2$s'),
  ('where not',              'SELECT count(*), sum(id) FROM %1$s WHERE NOT (%2$s)'),
  ('where is not true',      'SELECT count(*), sum(id) FROM %1$s WHERE (%2$s) IS NOT TRUE'),
  ('where is false',         'SELECT count(*), sum(id) FROM %1$s WHERE (%2$s) IS FALSE'),
  ('where or',               'SELECT count(*), sum(id) FROM %1$s WHERE %2$s OR g = 3'),
  ('where not or',           'SELECT count(*), sum(id) FROM %1$s WHERE NOT (%2$s OR g = 3)'),
  ('where is not true and',  'SELECT count(*), sum(id) FROM %1$s WHERE ((%2$s) AND g < 3) IS NOT TRUE'),
  ('where is not true or',   'SELECT count(*), sum(id) FROM %1$s WHERE ((%2$s) OR g < 3) IS NOT TRUE AND id > 100'),
  ('filter',                 'SELECT g, count(*) FILTER (WHERE %2$s), sum(id) FILTER (WHERE %2$s) FROM %1$s GROUP BY g'),
  ('filter not',             'SELECT g, count(*) FILTER (WHERE NOT (%2$s)) FROM %1$s GROUP BY g'),
  ('filter is not true',     'SELECT g, count(*) FILTER (WHERE (%2$s) IS NOT TRUE), sum(id) FILTER (WHERE (%2$s) IS NOT TRUE) FROM %1$s GROUP BY g'),
  ('case else 0',            'SELECT g, sum(CASE WHEN %2$s THEN 1 ELSE 0 END), count(CASE WHEN %2$s THEN id END) FROM %1$s GROUP BY g'),
  ('case else value',        'SELECT g, sum(CASE WHEN %2$s THEN id ELSE 1 END), min(CASE WHEN %2$s THEN id ELSE -id END) FROM %1$s GROUP BY g'),
  ('case pivot',             'SELECT count(*) AS total, sum(CASE WHEN %2$s THEN 1 ELSE 0 END) AS hits, sum(CASE WHEN %2$s THEN id ELSE 0 END) AS hit_ids, max(CASE WHEN %2$s THEN id END) AS last_hit, count(CASE WHEN %2$s THEN id END) AS hit_rows FROM %1$s'),
  ('case pivot by group',    'SELECT g, sum(CASE WHEN %2$s THEN 1 ELSE 0 END) AS hits, sum(CASE WHEN (%2$s) AND id > 500 THEN id ELSE 0 END) AS late_ids, avg(CASE WHEN %2$s THEN id END) AS mean_id FROM %1$s GROUP BY g'),
  ('case in key',            'SELECT CASE WHEN %2$s THEN 1 ELSE 0 END AS k, count(*) FROM %1$s GROUP BY 1'),
  ('case with filter',       'SELECT g, sum(CASE WHEN %2$s THEN id ELSE 0 END) FILTER (WHERE id %% 2 = 0) FROM %1$s GROUP BY g');

-- jw: a nullable document; jn: NOT NULL; jd: documents that are no object, a leaf cannot store them (the scan is not used)
CREATE TABLE qs AS SELECT row_number() OVER (ORDER BY tb.ord, t.ord, f.ord) AS n, tb.name || ': ' || t.name || ', ' || f.name AS label, format(f.tmpl, tb.name, t.cond) AS q
FROM tests t, forms f, (VALUES (1, 'jw'), (2, 'jn'), (3, 'jd')) tb(ord, name);
SELECT n, label, sift_helpers.agg_check_outcome(q) FROM qs ORDER BY n;

-- the rows themselves, not only equal: the statements the scan runs give these counts
SELECT count(*) AS total, count(*) FILTER (WHERE doc @> '{"b": "x"}') AS matches, count(*) FILTER (WHERE (doc @> '{"b": "x"}') IS NOT TRUE) AS not_true,
       count(*) FILTER (WHERE NOT (doc @> '{"b": "x"}')) AS is_false, count(*) FILTER (WHERE doc IS NULL) AS null_docs FROM jw;
SELECT count(*) FILTER (WHERE doc @> '{"b": "x"}') AS matches, count(*) FILTER (WHERE (doc @> '{"b": "x"}') IS NOT TRUE) AS not_true,
       count(*) FILTER (WHERE NOT (doc @> '{"b": "x"}')) AS is_false FROM jn;

DROP TABLE qs, tests, forms, jw, jn, jd;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
