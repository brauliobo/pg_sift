-- doc ? 'k', doc ?| array[..] and doc ?& array[..] on the document of an index that declares the paths as leaves are answered by the index wherever the test is: a branch of an OR
-- or an AND, the FILTER of an aggregate, a CASE (a FILTER and its IS NOT TRUE), NOT and IS NOT TRUE, not only as a conjunct of the WHERE. The source gives the rows that pass as a bitmap
-- of the unit and never reads the value of the leaf, so the leaf must not be read besides (such a query keeps the normal plan; a NOT of a document that may be NULL reads
-- the presence leaf, which tells a NULL document from a missing key). Every query runs with and without the scan and the rows (or the error) must be the same, for documents of every JSON type
-- at the root, a JSON null and a NULL document, keys with a null value, nested keys (not a match), the empty key, a key of non ASCII letters, values the leaf cannot hold and
-- large documents. The empty key cannot be declared as a path: its tests keep the normal plan.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET sift.agg_force = on;
SET sift.col_auto_merge = off;

CREATE FUNCTION shape(i int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS
$$ SELECT (ARRAY['{"k":1}', '{"k":null}', '{"j":1}', '{}', '["k"]', '["j"]', '["j","k",1]', '[1,{"k":2}]', '"k"', '"j"', '5', 'true', 'null', '[]',
	'{"k":{"a":1}}', '{"k":[1]}', '{"k":"str"}', '{"k":1.5}', '{"k":true}', '{"j":1,"k":null}', '["k","k"]', '"kk"', '{"K":1}', '{"k":2,"j":3}',
	'{"":1}', '{"é":1}', '{"a":{"k":1}}', '["é"]', '[""]', '"é"', '{"j":2,"":null}', '{"é":null,"k":3}'])[1 + i % 32]::jsonb
	|| CASE WHEN i % 97 = 0 AND (i % 32) IN (0, 2, 3) THEN jsonb_build_object('pad', repeat('x', 3000)) ELSE '{}' END $$;

-- a document column that may be NULL, and one that is NOT NULL (NOT of a test needs that)
CREATE TABLE kn (id int, g int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO kn SELECT i, i % 5, shape(i) FROM generate_series(1, 3200) i;
INSERT INTO kn VALUES (3201, 0, NULL), (3202, 1, NULL);
CREATE INDEX kn_col ON kn USING sift_col (id, g, doc) WITH (paths = 'doc.k int4, doc.j int4, doc."é" int4');
CREATE TABLE kc (id int, g int, doc jsonb NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO kc SELECT i, i % 5, shape(i) FROM generate_series(1, 3200) i;
CREATE INDEX kc_col ON kc USING sift_col (id, g, doc) WITH (paths = 'doc.k int4, doc.j int4, doc."é" int4');
-- the segments of an older library, which do not flag the documents that are a string or an array: every NULL slot is read again
CREATE TABLE ko (id int, g int, doc jsonb NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO ko SELECT i, i % 5, shape(i) FROM generate_series(1, 3200) i;
SET sift.test_no_roots = on;
CREATE INDEX ko_col ON ko USING sift_col (id, g, doc) WITH (paths = 'doc.k int4, doc.j int4, doc."é" int4');
RESET sift.test_no_roots;
VACUUM ANALYZE kn, kc, ko;

CREATE FUNCTION kf_rows(q text, push bool) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE r jsonb;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT jsonb_agg(to_jsonb(q) ORDER BY to_jsonb(q)::text) FROM (%s) q', q) INTO r;
	RETURN r;
EXCEPTION WHEN OTHERS THEN
	RETURN to_jsonb('error: ' || SQLERRM);
END $$;

-- the text of an error is not printed (before PostgreSQL 18 the cast of a JSON null raises, so the first row that raises differs), only that both plans raise the same one;
-- the plan is not printed for the forms whose plan PostgreSQL 19 changes by itself (count(doc) of a NOT NULL column is count(*), doc IS NULL of one is false)
CREATE FUNCTION kf_check(q text, pin bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a jsonb; b jsonb; l text; pushed bool := false;
BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		pushed := pushed OR l LIKE '%SiftAgg%';
	END LOOP;
	a := kf_rows(q, true);
	b := kf_rows(q, false);
	RETURN CASE WHEN a IS DISTINCT FROM b THEN 'DIFFERENT' WHEN a #>> '{}' LIKE 'error: %' THEN 'same error' ELSE 'same ' || left(coalesce(a::text, '-'), 70) END ||
		CASE WHEN NOT pin THEN '' WHEN pushed THEN ' pushed' ELSE ' normal' END;
END $$;

CREATE TABLE kf_q (n text PRIMARY KEY, q text, pin bool NOT NULL DEFAULT true);
INSERT INTO kf_q (n, q) VALUES
	('01 where',             'SELECT count(*), sum(id) FROM @ WHERE doc ? ''k'''),
	('02 filter',            'SELECT count(*) FILTER (WHERE doc ? ''k''), count(*) FROM @'),
	('03 two filters',       'SELECT count(*) FILTER (WHERE doc ? ''k''), sum(id) FILTER (WHERE doc ? ''j''), avg(id) FILTER (WHERE doc ? ''k'') FROM @'),
	('04 case sum else 0',   'SELECT sum(CASE WHEN doc ? ''k'' THEN id ELSE 0 END) FROM @'),
	('05 case count',        'SELECT count(CASE WHEN doc ? ''k'' THEN 1 END), count(CASE WHEN doc ? ''k'' THEN NULL ELSE 1 END) FROM @'),
	('06 case avg',          'SELECT avg(CASE WHEN doc ? ''k'' THEN id END), avg(CASE WHEN doc ? ''k'' THEN id ELSE 0 END) FROM @'),
	('07 or',                'SELECT count(*) FROM @ WHERE doc ? ''k'' OR g = 1'),
	('08 or two keys',       'SELECT count(*) FROM @ WHERE doc ? ''k'' OR doc ? ''j'''),
	('09 and or',            'SELECT count(*) FROM @ WHERE (doc ? ''k'' OR doc ? ''j'') AND g <> 2'),
	('10 filter and',        'SELECT count(*) FILTER (WHERE doc ? ''k'' AND g = 2), count(*) FILTER (WHERE doc ? ''k'' OR g = 2) FROM @'),
	('11 not',               'SELECT count(*) FROM @ WHERE NOT (doc ? ''k'')'),
	('12 not in a filter',   'SELECT count(*) FILTER (WHERE NOT (doc ? ''k'')), count(*) FILTER (WHERE NOT (doc ? ''j'') AND g = 1) FROM @'),
	('13 is not true',       'SELECT count(*) FROM @ WHERE (doc ? ''k'') IS NOT TRUE'),
	('14 is not true filter', 'SELECT count(*) FILTER (WHERE (doc ? ''k'') IS NOT TRUE), count(*) FILTER (WHERE (doc ? ''k'' OR doc ? ''j'') IS NOT TRUE) FROM @'),
	('15 any where',         'SELECT count(*) FROM @ WHERE doc ?| array[''k'', ''j'']'),
	('16 any filter',        'SELECT count(*) FILTER (WHERE doc ?| array[''k'', ''j'']), sum(id) FILTER (WHERE doc ?| array[''k'', ''j'', ''k'']) FROM @'),
	('17 all filter',        'SELECT count(*) FILTER (WHERE doc ?& array[''k'', ''j'']), count(*) FILTER (WHERE doc ?& array[''k'', ''k'']) FROM @'),
	('18 all or',            'SELECT count(*) FROM @ WHERE doc ?& array[''k'', ''j''] OR g = 3'),
	('19 not any',           'SELECT count(*) FROM @ WHERE NOT (doc ?| array[''k'', ''j''])'),
	('20 not all',           'SELECT count(*) FROM @ WHERE NOT (doc ?& array[''k'', ''j''])'),
	('21 case any',          'SELECT sum(CASE WHEN doc ?| array[''k'', ''j''] THEN 1 ELSE 0 END), sum(CASE WHEN doc ?& array[''k'', ''j''] THEN id ELSE 0 END) FROM @'),
	('22 groups',            'SELECT g, count(*) FILTER (WHERE doc ? ''k''), sum(CASE WHEN doc ? ''j'' THEN id ELSE 0 END) FROM @ GROUP BY g'),
	('23 groups where',      'SELECT g, count(*) FROM @ WHERE doc ? ''k'' OR doc ? ''j'' GROUP BY g'),
	('24 empty key',         'SELECT count(*) FILTER (WHERE doc ? ''''), count(*) FILTER (WHERE doc ?| array['''', ''k'']) FROM @'),
	('25 unicode key',       'SELECT count(*) FILTER (WHERE doc ? ''é''), count(*) FILTER (WHERE doc ?& array[''é'', ''k'']), count(*) FROM @ WHERE doc ? ''é'' OR g = 0'),
	('26 undeclared key',    'SELECT count(*) FILTER (WHERE doc ? ''zz''), count(*) FROM @'),
	('27 one undeclared',    'SELECT count(*) FROM @ WHERE doc ? ''k'' OR doc ? ''zz'''),
	('28 any one undeclared', 'SELECT count(*) FILTER (WHERE doc ?| array[''k'', ''zz'']) FROM @'),
	('29 read of the key',   'SELECT sum((doc ->> ''k'')::numeric) FILTER (WHERE doc ? ''k'') FROM @'),
	('30 read of the key in a case', 'SELECT sum(CASE WHEN doc ? ''k'' THEN (doc -> ''k'')::numeric ELSE 0 END) FROM @'),
	('31 read of the document', 'SELECT count(*) FILTER (WHERE doc ? ''k''), count(doc) FROM @'),
	('32 read of another key', 'SELECT count(*) FILTER (WHERE doc ? ''k''), sum((doc ->> ''j'')::int) FROM @'),
	('33 type test beside',  'SELECT count(*) FILTER (WHERE doc ? ''k'' OR jsonb_typeof(doc -> ''k'') = ''number'') FROM @'),
	('34 type test forms',   'SELECT count(*) FILTER (WHERE jsonb_typeof(doc -> ''k'') = ''number''), count(*) FILTER (WHERE jsonb_typeof(doc -> ''k'') IS NULL), count(*) FROM @ WHERE jsonb_typeof(doc -> ''k'') = ''string'' OR g = 1'),
	('35 type case',         'SELECT sum(CASE WHEN jsonb_typeof(doc -> ''k'') IN (''number'', ''null'') THEN id ELSE 0 END) FROM @'),
	('36 type guard read',   'SELECT sum(CASE WHEN jsonb_typeof(doc -> ''k'') = ''number'' THEN (doc ->> ''k'')::numeric ELSE 0 END) FROM @'),
	('37 key in an output',  'SELECT count(*) FILTER (WHERE doc ? ''k''), bool_or(doc ? ''k'') FROM @'),
	('38 no document',       'SELECT count(*) FILTER (WHERE doc IS NULL OR doc ? ''k'') FROM @');
UPDATE kf_q SET pin = false WHERE n IN ('31 read of the document', '38 no document');
CREATE FUNCTION kf_run(tbl text) RETURNS TABLE (n text, verdict text) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY EXECUTE format('SELECT n, kf_check(replace(q, ''@'', %L), pin) FROM kf_q ORDER BY n', tbl);
END $$;

-- the document that may be NULL: a NULL document has no key, and NOT of the test of it is NULL, which keeps no row
SELECT * FROM kf_run('kn');
-- the document that is NOT NULL: NOT of the test is the complement
SELECT * FROM kf_run('kc');

SELECT * FROM kf_run('ko');

-- writes the index holds in its buffer: new rows of every shape, rows that change their shape, deleted rows
DO $$
DECLARE t text;
BEGIN
	FOREACH t IN ARRAY ARRAY['kn', 'kc', 'ko'] LOOP
		EXECUTE format('INSERT INTO %I SELECT i, i %% 5, shape(i + 7) FROM generate_series(3301, 3500) i', t);
		EXECUTE format('UPDATE %I SET doc = shape(id + 5) WHERE id %% 7 = 0', t);
		EXECUTE format('DELETE FROM %I WHERE id %% 11 = 0', t);
	END LOOP;
END $$;
SELECT * FROM kf_run('kn');
SELECT * FROM kf_run('kc');
SELECT * FROM kf_run('ko');

-- promoted and vacuumed
SELECT sift.col_promote('kn_col'), sift.col_promote('kc_col'), sift.col_promote('ko_col');
VACUUM kn, kc, ko;
SELECT * FROM kf_run('kn');
SELECT * FROM kf_run('kc');
SELECT * FROM kf_run('ko');

-- the workers test the same: the scan plans two workers, and the rows are the same
CREATE TABLE kp (id int, g int, doc jsonb NOT NULL) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO kp SELECT i, i % 5, shape(i) FROM generate_series(1, 12000) i;
CREATE INDEX kp_col ON kp USING sift_col (id, g, doc) WITH (paths = 'doc.k int4, doc.j int4, doc."é" int4');
VACUUM ANALYZE kp;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
CREATE FUNCTION kf_workers(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; g bool := false;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		g := g OR l LIKE '%Workers Planned: 2%';
	END LOOP;
	RETURN g;
END $$;
SELECT kf_workers('SELECT count(*) FILTER (WHERE doc ? ''k''), sum(id) FILTER (WHERE doc ?| array[''k'', ''j'']) FROM kp') AS workers_planned;
SELECT * FROM kf_run('kp');

DROP TABLE kf_q;
DROP FUNCTION kf_run(text), kf_check(text, bool), kf_rows(text, bool), kf_workers(text);
DROP TABLE kn, kc, ko, kp;
DROP FUNCTION shape(int);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
