-- The key tests of a jsonb document of a sift_table beside a test of another column: ?, ?|, ?&, NOT ?, @> and ->> over every shape of document (an object with the key, its JSON null,
-- a value its declared path cannot hold, an array and a string that hold the key, a SQL NULL). The leaves of a table keep no presence of their key: ? is no test of the scan, the node
-- reads it from the document. Each statement runs on both formats against a heap twin (the truth): the first word says that the answers agree, the second whether the plan is the node's.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;
SELECT current_setting('sift.test_create_format1', true) IS NOT NULL AS testing \gset

CREATE FUNCTION same(q text, t text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; line text; node boolean := false;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || replace(q, '@T', t) LOOP node := node OR line LIKE '%SiftAgg%'; END LOOP;
	EXECUTE format('SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (%s) r', replace(q, '@T', t)) INTO a;
	EXECUTE format('SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (%s) r', replace(q, '@T', 'heap_docs')) INTO b;
	RETURN (CASE WHEN a = b THEN 'same' ELSE 'DIFFERENT' END) || ' ' || (CASE WHEN node THEN 'node' ELSE 'normal' END) || ' ' || replace(q, '@T', 'T');
END $$;

CREATE TABLE heap_docs AS SELECT i AS id, i % 10 AS n, CASE i % 7 WHEN 0 THEN NULL WHEN 1 THEN '{"b": "x"}' WHEN 2 THEN '["a", "z"]' WHEN 3 THEN '"a"'
	WHEN 4 THEN '{"a": null}' WHEN 5 THEN '{"a": "str"}' ELSE jsonb_build_object('a', i, 'b', 'y') END::jsonb AS doc FROM generate_series(1, 20000) i;
CREATE TABLE t2 (id int, n int, doc jsonb) USING sift_table;
SELECT sift.table_set_paths('t2', 'doc', 'doc.a float8, doc.b astext');
INSERT INTO t2 SELECT * FROM heap_docs;
\if :testing
SET sift.test_create_format1 = on;
\endif
CREATE TABLE t1 (id int, n int, doc jsonb) USING sift_table;
SELECT sift.table_set_paths('t1', 'doc', 'doc.a float8, doc.b astext');
INSERT INTO t1 SELECT * FROM heap_docs;
\if :testing
RESET sift.test_create_format1;
\endif
VACUUM (FREEZE, ANALYZE) heap_docs, t1, t2;

CREATE TEMP TABLE queries (q text);
INSERT INTO queries VALUES
	('SELECT count(*) FROM @T WHERE doc ? ''a'' AND n < 5'),
	('SELECT n, count(*) FROM @T WHERE doc ? ''a'' AND id BETWEEN 100 AND 15000 GROUP BY n'),
	('SELECT count(*) FROM @T WHERE doc ?| ''{a,q}'' AND n < 5'),
	('SELECT count(*) FROM @T WHERE doc ?& ''{a,b}'' AND n < 5'),
	('SELECT count(*) FROM @T WHERE NOT (doc ? ''a'') AND n < 5'),
	('SELECT count(*) FROM @T WHERE doc @> ''{"b": "y"}'' AND n < 5'),
	('SELECT count(*) FROM @T WHERE doc->>''b'' = ''y'' AND n < 5'),
	('SELECT count(*), sum((doc->>''a'')::float8) FROM @T WHERE jsonb_typeof(doc->''a'') = ''number'' AND n < 5');
SELECT same(q, 't2') AS format2 FROM queries ORDER BY q;
SELECT same(q, 't1') AS format1 FROM queries ORDER BY q;
DROP TABLE heap_docs, t1, t2;
DROP FUNCTION same(text, text);
DROP EXTENSION pg_sift;
