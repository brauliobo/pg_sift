-- The jsonb leaves of sift_colstub (SIFT_TESTING builds): scalar paths, arrays of objects and objects with free keys decode to what
-- the SQL expressions (->>, jsonb_array_elements, jsonb_each) say, NULLs and odd shapes included; a value of another JSON type is
-- cast as (x #>> '{}')::type is.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET datestyle = 'ISO, MDY';
SET timezone = 'UTC';

-- the dump of a leaf against a query that returns (tid, text)
CREATE FUNCTION qdiffs(idx regclass, leaf int, q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM ((SELECT tid, val FROM sift._test_aggstub_dump(%L, %s) EXCEPT ALL %s)
					UNION ALL (%s EXCEPT ALL SELECT tid, val FROM sift._test_aggstub_dump(%L, %s))) x', idx, leaf, q, q, idx, leaf) INTO n;
	RETURN n;
END $$;

-- the shapes: normal, empty, SQL NULL, JSON null, other JSON types, elements that are not objects, fields of other types
CREATE TEMP TABLE pat (k int, metrics jsonb, items jsonb, notes jsonb, details jsonb);
INSERT INTO pat VALUES
 (1, '{"total": 8.5, "shipping": 7, "tax_vat": 9}',
	 '[{"category":"Garden","sku":"bulb","kind":"credit","qty":2,"price":8.2,"discount":0.1},{"category":"Home","sku":"lamp","kind":"debit","qty":1,"price":20}]',
	 '[{"kind":"gift","text":"for you"}]',
	 '{"lang":"en","mapped":true,"nested":{"when":"2024-03-01","at":"2024-03-01 10:20:30","n":12345678901,"f":1.5}}'),
 (2, '{}', '[]', '[]', '{}'),
 (3, NULL, NULL, NULL, NULL),
 (4, 'null', 'null', 'null', 'null'),
 (5, '[1,2]', '{"a":1}', '"x"', '[1]'),
 (6, '"str"', '5', '5', '"text"'),
 (7, '5', '[1, "x", null, [1,2], {"sku":"a","qty":"7","price":"3.25"}, {}, {"category": null, "qty": null, "price": 1e2}]',
	 '[{"kind":"a"},{"kind":"b"},{"text":"c"}]',
	 '{"lang": 5, "mapped": "true", "nested": {"when": "2024-12-31", "at": "2024-12-31 23:59:59.5", "n": "42", "f": "2.5"}}'),
 (8, '{"a": null, "b": "7.5", "c": 2, "d": "1e3", "zz": -0.5, "": 1.25, "ünï": 3}',
	 '[{"category":"Garden","sku":"x","kind":"credit","qty":-3,"price":0}]',
	 '[]',
	 '{"lang": {"x": [1, "b"]}, "mapped": 0, "nested": [1]}'),
 (9, '{"x": {"y": 1}, "z": null}'::jsonb - 'x', '{"items": []}', '[{"kind": null, "text": null}]',
	 '{"lang": true, "mapped": null, "nested": {"when": null, "at": null, "n": null, "f": null}}'),
 (10, '{"p": 1, "q": 2, "r": 3, "s": 4}', '[{"category":"Home","sku":"lamp","kind":"debit","qty":5,"price":0.25},{"category":"Home","sku":"lamp","kind":"debit","qty":6,"price":0.5},{"category":"Garden","sku":"seed","kind":"debit","qty":0,"price":3}]',
	 '[{"kind":"gift"}]', '{"lang": "de", "mapped": "t", "nested": "gone"}');

-- about 1500 rows in several superblocks
CREATE TABLE o (id int, metrics jsonb, items jsonb, notes jsonb, details jsonb, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO o SELECT (g - 1) * 10 + p.k, p.metrics, p.items, p.notes, p.details, repeat('x', 200) FROM generate_series(1, 150) g, pat p ORDER BY g, p.k;
CREATE INDEX o_col ON o USING sift_colstub (id, metrics, items, notes, details)
	WITH (paths = 'metrics: *:float8;
				   items: []{category text, sku text, kind text, qty int4, price float8};
				   notes: []{kind text, text text};
				   details: lang text, mapped bool, nested.when date, nested.at timestamp, nested.n int8, nested.f float8');
SELECT count(*) > 3 AS several_units FROM sift._test_aggstub_units('o_col');
SELECT leaf, attno, parent, kind, vtype, typid, path, name FROM sift._test_aggstub_layout('o_col') ORDER BY leaf;

-- scalar paths: -> then ->> and the cast of the declared type
SELECT x.leaf, x.name, qdiffs('o_col', x.leaf, format('SELECT ctid, (%s)::text FROM o', x.expr)) AS diffs
FROM (VALUES (13, 'lang', $$details->>'lang'$$),
			 (14, 'mapped', $$substr(((details->>'mapped')::bool)::text, 1, 1)$$),
			 (15, 'nested.when', $$(details->'nested'->>'when')::date$$),
			 (16, 'nested.at', $$(details->'nested'->>'at')::timestamp$$),
			 (17, 'nested.n', $$(details->'nested'->>'n')::int8$$),
			 (18, 'nested.f', $$(details->'nested'->>'f')::float8$$)) x(leaf, name, expr)
ORDER BY x.leaf;
-- some of the values they hold
SELECT val AS lang, count(*) FROM sift._test_aggstub_dump('o_col', 13) GROUP BY val ORDER BY val COLLATE "C" NULLS LAST;
SELECT val AS mapped, count(*) FROM sift._test_aggstub_dump('o_col', 14) GROUP BY val ORDER BY val NULLS LAST;
SELECT val AS nested_n, count(*) FROM sift._test_aggstub_dump('o_col', 17) GROUP BY val ORDER BY val::int8 NULLS LAST;

-- the free keys of an object: the count (NULL when the column is not an object), then the pairs
SELECT qdiffs('o_col', 1, $$SELECT ctid, CASE WHEN jsonb_typeof(metrics) = 'object' THEN (SELECT count(*) FROM jsonb_object_keys(metrics)) END::text FROM o$$) AS counts;
SELECT count(*) FILTER (WHERE val IS NULL) AS absent, count(*) FILTER (WHERE val = '0') AS empty, count(*) FILTER (WHERE val::int > 0) AS with_keys
FROM sift._test_aggstub_dump('o_col', 1);
SELECT qdiffs('o_col', 2, $$SELECT o.ctid, e.key FROM o, jsonb_each(CASE WHEN jsonb_typeof(o.metrics) = 'object' THEN o.metrics END) e$$) AS keys;
SELECT qdiffs('o_col', 3, $$SELECT o.ctid, (e.value #>> '{}')::float8::text FROM o, jsonb_each(CASE WHEN jsonb_typeof(o.metrics) = 'object' THEN o.metrics END) e$$) AS vals;
-- the key and the value of a pair are the same element
SELECT count(*) AS pair_diffs FROM (
	(SELECT k.tid, k.elem - min(k.elem) OVER (PARTITION BY k.unit, k.ord) + 1 AS n, k.val, v.val FROM sift._test_aggstub_dump('o_col', 2) k JOIN sift._test_aggstub_dump('o_col', 3) v USING (unit, elem)
	 EXCEPT ALL
	 SELECT o.ctid, e.n, e.v ->> 'k', (e.v ->> 'v')::float8::text FROM o,
		LATERAL (SELECT n, jsonb_build_object('k', key, 'v', value) AS v FROM jsonb_each(CASE WHEN jsonb_typeof(o.metrics) = 'object' THEN o.metrics END) WITH ORDINALITY AS t(key, value, n)) e)
	UNION ALL
	(SELECT o.ctid, e.n, e.v ->> 'k', (e.v ->> 'v')::float8::text FROM o,
		LATERAL (SELECT n, jsonb_build_object('k', key, 'v', value) AS v FROM jsonb_each(CASE WHEN jsonb_typeof(o.metrics) = 'object' THEN o.metrics END) WITH ORDINALITY AS t(key, value, n)) e
	 EXCEPT ALL
	 SELECT k.tid, k.elem - min(k.elem) OVER (PARTITION BY k.unit, k.ord) + 1, k.val, v.val FROM sift._test_aggstub_dump('o_col', 2) k JOIN sift._test_aggstub_dump('o_col', 3) v USING (unit, elem))) x;
SELECT val AS key, count(*) FROM sift._test_aggstub_dump('o_col', 2) GROUP BY val ORDER BY val COLLATE "C";

-- an array of objects: the count (NULL: a missing key, JSON null or not an array), then the fields of the elements
SELECT qdiffs('o_col', 4, $$SELECT ctid, CASE WHEN jsonb_typeof(items) = 'array' THEN jsonb_array_length(items) END::text FROM o$$) AS counts;
SELECT val AS elements, count(*) FROM sift._test_aggstub_dump('o_col', 4) GROUP BY val ORDER BY val::int NULLS LAST;
SELECT x.leaf, x.name, qdiffs('o_col', x.leaf, format('SELECT o.ctid, (e.v->>%L)::%s::text FROM o, jsonb_array_elements(CASE WHEN jsonb_typeof(o.items) = %L THEN o.items END) e(v)',
													 x.name, x.type, 'array')) AS diffs
FROM (VALUES (5, 'category', 'text'), (6, 'sku', 'text'), (7, 'kind', 'text'), (8, 'qty', 'int4'), (9, 'price', 'float8')) x(leaf, name, type)
ORDER BY x.leaf;
-- the fields of an element belong together (same-element semantics of jsonb_array_elements)
SELECT count(*) AS element_diffs FROM (
	(SELECT c.tid, c.elem - min(c.elem) OVER (PARTITION BY c.unit, c.ord) + 1 AS n, c.val, s.val, k.val, q.val, p.val
	 FROM sift._test_aggstub_dump('o_col', 5) c JOIN sift._test_aggstub_dump('o_col', 6) s USING (unit, elem) JOIN sift._test_aggstub_dump('o_col', 7) k USING (unit, elem)
		  JOIN sift._test_aggstub_dump('o_col', 8) q USING (unit, elem) JOIN sift._test_aggstub_dump('o_col', 9) p USING (unit, elem)
	 EXCEPT ALL
	 SELECT o.ctid, e.n, e.v ->> 'category', e.v ->> 'sku', e.v ->> 'kind', (e.v ->> 'qty')::int4::text, (e.v ->> 'price')::float8::text
	 FROM o, jsonb_array_elements(CASE WHEN jsonb_typeof(o.items) = 'array' THEN o.items END) WITH ORDINALITY e(v, n))
	UNION ALL
	(SELECT o.ctid, e.n, e.v ->> 'category', e.v ->> 'sku', e.v ->> 'kind', (e.v ->> 'qty')::int4::text, (e.v ->> 'price')::float8::text
	 FROM o, jsonb_array_elements(CASE WHEN jsonb_typeof(o.items) = 'array' THEN o.items END) WITH ORDINALITY e(v, n)
	 EXCEPT ALL
	 SELECT c.tid, c.elem - min(c.elem) OVER (PARTITION BY c.unit, c.ord) + 1, c.val, s.val, k.val, q.val, p.val
	 FROM sift._test_aggstub_dump('o_col', 5) c JOIN sift._test_aggstub_dump('o_col', 6) s USING (unit, elem) JOIN sift._test_aggstub_dump('o_col', 7) k USING (unit, elem)
		  JOIN sift._test_aggstub_dump('o_col', 8) q USING (unit, elem) JOIN sift._test_aggstub_dump('o_col', 9) p USING (unit, elem))) x;
-- the elements of one row (a document with a bit of everything) as the decoder gives them
SELECT c.ord, c.elem - min(c.elem) OVER (PARTITION BY c.unit, c.ord) AS n, c.val AS category, s.val AS sku, k.val AS kind, q.val AS qty, p.val AS price
FROM sift._test_aggstub_dump('o_col', 5, 0, 1) c JOIN sift._test_aggstub_dump('o_col', 6, 0, 1) s USING (unit, elem) JOIN sift._test_aggstub_dump('o_col', 7, 0, 1) k USING (unit, elem)
	 JOIN sift._test_aggstub_dump('o_col', 8, 0, 1) q USING (unit, elem) JOIN sift._test_aggstub_dump('o_col', 9, 0, 1) p USING (unit, elem)
WHERE c.ord < 10 ORDER BY c.ord, c.elem;

-- a second array leaf of the same index
SELECT qdiffs('o_col', 10, $$SELECT ctid, CASE WHEN jsonb_typeof(notes) = 'array' THEN jsonb_array_length(notes) END::text FROM o$$) AS counts;
SELECT qdiffs('o_col', 11, $$SELECT o.ctid, e.v->>'kind' FROM o, jsonb_array_elements(CASE WHEN jsonb_typeof(o.notes) = 'array' THEN o.notes END) e(v)$$) AS kinds;
SELECT qdiffs('o_col', 12, $$SELECT o.ctid, e.v->>'text' FROM o, jsonb_array_elements(CASE WHEN jsonb_typeof(o.notes) = 'array' THEN o.notes END) e(v)$$) AS texts;

-- dictionaries of the child leaves are per unit too
SELECT count(*) = count(DISTINCT id) AS ids_unique, count(*) = (SELECT count(*) FROM sift._test_aggstub_units('o_col')) * 7 AS text_leaves_per_unit
FROM sift._test_aggstub_dicts('o_col');

-- the leaves read from the same attribute, in any order and repeatedly
SELECT count(*) AS rows_of_sb1 FROM sift._test_aggstub_dump('o_col', 9, 1, 2);
SELECT units, rows = (SELECT count(*) FROM o) AS rows, bytes > 0 AS bytes FROM sift._test_aggstub_counters('o_col');

-- values the cast refuses, found by CREATE INDEX with the error of the SQL cast
CREATE TABLE e (id int, j jsonb);
INSERT INTO e VALUES (1, '{"a": 7.5}');
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: a int4');
SELECT ('{"a": 7.5}'::jsonb #>> '{a}')::int4;
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: a float8');
SELECT val FROM sift._test_aggstub_dump('e_col', 1);
DROP INDEX e_col;
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: a text');
SELECT val FROM sift._test_aggstub_dump('e_col', 1);
DROP INDEX e_col;
TRUNCATE e;
INSERT INTO e VALUES (1, '{"a": true}');
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: a float8');
SELECT ('{"a": true}'::jsonb #>> '{a}')::float8;
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: a int8');
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: a date');
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: a timestamp');
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: a bool, b.c int4, d.e text');
SELECT 1 AS leaf, val FROM sift._test_aggstub_dump('e_col', 1) UNION ALL SELECT 2, val FROM sift._test_aggstub_dump('e_col', 2) ORDER BY 1;
DROP INDEX e_col;
TRUNCATE e;
INSERT INTO e VALUES (1, '{"a": "yes", "b": "01/02/2024", "c": "off"}');
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: a bool, b date, c bool');
SELECT ('{"a": "yes", "b": "01/02/2024"}'::jsonb #>> '{a}')::bool AS a, ('{"a": "yes", "b": "01/02/2024"}'::jsonb #>> '{b}')::date AS b;
SELECT 1 AS leaf, val FROM sift._test_aggstub_dump('e_col', 1) UNION ALL SELECT 2, val FROM sift._test_aggstub_dump('e_col', 2)
UNION ALL SELECT 3, val FROM sift._test_aggstub_dump('e_col', 3) ORDER BY 1;
DROP INDEX e_col;
TRUNCATE e;
INSERT INTO e VALUES (1, '{"a": {"b": "x"}}');
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: a.b float8');
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: *:int4');
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: a[]{b text}');
SELECT val AS count FROM sift._test_aggstub_dump('e_col', 1);
DROP INDEX e_col;
TRUNCATE e;
INSERT INTO e VALUES (1, '{"a": [{"b": "x"}, {"b": 1.5}]}');
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: a[]{b int4}');
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: a[]{b text}');
SELECT elem, val FROM sift._test_aggstub_dump('e_col', 2) ORDER BY elem;
DROP INDEX e_col;
-- a root that is an array
TRUNCATE e;
INSERT INTO e VALUES (1, '[{"b": 1}, {"b": 2}, 3]'), (2, '{"b": 1}'), (3, '7'), (4, NULL);
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: []{b int4}');
SELECT ord, elem, val FROM sift._test_aggstub_dump('e_col', 1) ORDER BY ord;
SELECT ord, elem, val FROM sift._test_aggstub_dump('e_col', 2) ORDER BY ord, elem;
DROP INDEX e_col;

-- 255 bytes of text are the limit, in a value, a key and a field of an element
TRUNCATE e;
INSERT INTO e VALUES (1, jsonb_build_object('a', repeat('k', 255), 'l', jsonb_build_array(jsonb_build_object('t', repeat('é', 127)))));
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: a text, l[]{t text}');
DROP INDEX e_col;
TRUNCATE e;
INSERT INTO e VALUES (2, jsonb_build_object('a', repeat('k', 256)));
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: a text');
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: *:text');
TRUNCATE e;
INSERT INTO e VALUES (2, jsonb_build_object(repeat('k', 256), 1));
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: *:float8');
TRUNCATE e;
INSERT INTO e VALUES (3, jsonb_build_object('l', jsonb_build_array(jsonb_build_object('t', repeat('é', 128)))));
CREATE INDEX e_col ON e USING sift_colstub (id, j) WITH (paths = 'j: l[]{t text}');

DROP TABLE o, e;
-- the shared database of a plain installcheck goes on with the next test
DROP TABLE pat;
SET client_min_messages = warning;
DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
GRANT USAGE ON SCHEMA public TO PUBLIC;
RESET client_min_messages;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
