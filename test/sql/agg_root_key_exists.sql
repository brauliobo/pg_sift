-- doc ? 'k' on the document of an index that declares the path k is pushed into the aggregate scan: the rows whose k holds a value of any JSON type (a JSON null too) are the
-- rows with the key, and the documents that are no object (an array holding the string, the string itself) say that of themselves, which the leaf does not store: the source
-- reads those rows again from the heap. Every query runs with and without the pushdown and the rows must be the same, for the documents of every JSON type at the root, for the
-- keys present, absent and JSON null (?& and ?| of keys that are leaves are several ?, a key that is no leaf keeps the normal plan; agg_jsonb_key_forms has the forms), through writes, UPDATE and DELETE, in a segment written before the segments flagged the string roots (sift.test_no_roots), next to one that
-- is, and after the VACUUM that flags the old ones.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET sift.agg_force = on;
SET sift.col_auto_merge = off;

-- 24 shapes of a document: objects with k (a number, a JSON null, an object, an array, a string, a boolean, a JSON null beside another key), objects without it, arrays that hold the
-- string and arrays that do not, strings that equal the key and strings that do not, a number, a boolean, a JSON null, an empty object, an empty array and a key of another case
CREATE FUNCTION shape(i int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS
$$ SELECT (ARRAY['{"k":1}', '{"k":null}', '{"j":1}', '{}', '["k"]', '["j"]', '["j","k",1]', '[1,{"k":2}]', '"k"', '"j"', '5', 'true', 'null', '[]',
	'{"k":{"a":1}}', '{"k":[1]}', '{"k":"str"}', '{"k":1.5}', '{"k":true}', '{"j":1,"k":null}', '["k","k"]', '"kk"', '{"K":1}', '{"k":2,"j":3}'])[1 + i % 24]::jsonb $$;
-- the same without the strings, whose documents the VACUUM that flags the old segments needs to be none of
CREATE FUNCTION shape_nostr(i int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS
$$ SELECT shape(CASE WHEN i % 24 IN (8, 9, 21) THEN i + 3 ELSE i END) $$;

CREATE TABLE hk (id int, g int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO hk SELECT i, i % 5, shape(i) FROM generate_series(1, 4800) i;
INSERT INTO hk VALUES (4801, 0, NULL);
CREATE INDEX hk_col ON hk USING sift_col (id, g, doc) WITH (paths = 'doc.k int4, doc.j int4');

-- the segments of an older library: no string root is flagged
CREATE TABLE ho (id int, g int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO ho SELECT i, i % 5, shape(i) FROM generate_series(1, 4800) i;
SET sift.test_no_roots = on;
CREATE INDEX ho_col ON ho USING sift_col (id, g, doc) WITH (paths = 'doc.k int4, doc.j int4');
RESET sift.test_no_roots;

-- ... one of them next to a segment that is flagged
CREATE TABLE hm (id int, g int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO hm SELECT i, i % 5, shape(i) FROM generate_series(1, 2400) i;
SET sift.test_no_roots = on;
CREATE INDEX hm_col ON hm USING sift_col (id, g, doc) WITH (paths = 'doc.k int4, doc.j int4');
RESET sift.test_no_roots;
INSERT INTO hm SELECT i, i % 5, shape(i) FROM generate_series(2401, 4800) i;
SELECT sift.col_promote('hm_col');

-- the old segments of a heap that has no document that is a string
CREATE TABLE hc (id int, g int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO hc SELECT i, i % 5, shape_nostr(i) FROM generate_series(1, 4800) i;
SET sift.test_no_roots = on;
CREATE INDEX hc_col ON hc USING sift_col (id, g, doc) WITH (paths = 'doc.k int4, doc.j int4');
RESET sift.test_no_roots;
VACUUM ANALYZE hk, ho, hm, hc;

CREATE FUNCTION hk_rows(q text, push bool) RETURNS jsonb[] LANGUAGE plpgsql AS $$
DECLARE r jsonb[];
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT array_agg(to_jsonb(q) ORDER BY to_jsonb(q)::text) FROM (%s) q', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION hk_check(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a jsonb[]; b jsonb[]; l text; pushed bool := false;
BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		pushed := pushed OR l LIKE '%SiftAgg%';
	END LOOP;
	a := hk_rows(q, true);
	b := hk_rows(q, false);
	RETURN CASE WHEN a IS NOT DISTINCT FROM b THEN 'same ' || coalesce(a[1]::text, '-') ELSE 'DIFFERENT' END || CASE WHEN pushed THEN ' pushed' ELSE ' normal' END;
END $$;

CREATE TABLE hk_q (n text PRIMARY KEY, q text);
INSERT INTO hk_q VALUES
	('1 count',        'SELECT count(*) FROM @ WHERE doc ? ''k'''),
	('2 filter',       'SELECT count(*) FROM @ WHERE doc ? ''k'' AND g = 2'),
	('3 sum',          'SELECT sum(id), min(id), max(id) FROM @ WHERE doc ? ''k'''),
	('4 groups',       'SELECT g, count(*) FROM @ WHERE doc ? ''k'' GROUP BY g'),
	('5 absent key',   'SELECT count(*) FROM @ WHERE doc ? ''zz'''),
	('6 second key',   'SELECT count(*) FROM @ WHERE doc ? ''j'''),
	('6b undeclared',  'SELECT count(*) FROM @ WHERE doc ? ''zz'' OR doc ? ''q'''),
	('7 not',          'SELECT count(*) FROM @ WHERE NOT (doc ? ''k'')'),
	('8 or',           'SELECT count(*) FROM @ WHERE doc ? ''k'' OR g = 1'),
	('9 two keys',     'SELECT count(*) FROM @ WHERE doc ? ''k'' AND doc ? ''j'''),
	('10 read too',    'SELECT count(*) FROM @ WHERE doc ? ''k'' AND (doc -> ''k'') IS NOT NULL'),
	('14 all',          'SELECT count(*) FROM @ WHERE doc ?& array[''k'', ''j'']'),
	('15 all, one undeclared', 'SELECT count(*) FROM @ WHERE doc ?& array[''k'', ''zz'']'),
	('11 any',         'SELECT count(*) FROM @ WHERE doc ?| array[''k'', ''zz'']'),
	('12 type test',   'SELECT count(*) FROM @ WHERE jsonb_typeof(doc -> ''k'') IS NOT NULL'),
	('13 no document', 'SELECT count(*) FROM @ WHERE doc IS NULL');
CREATE FUNCTION hk_run(tbl text) RETURNS TABLE (n text, verdict text) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY EXECUTE format('SELECT n, hk_check(replace(q, ''@'', %L)) FROM hk_q ORDER BY n', tbl);
END $$;

-- every shape: the rows that have the key by shape, for the new segments, the old ones, both, and the old ones of a heap with no string
SELECT * FROM hk_run('hk');
SELECT * FROM hk_run('ho');
SELECT * FROM hk_run('hm');
SELECT * FROM hk_run('hc');

-- what the flag says: the old segments of ho and hm are not flagged, those of hc are flagged by the VACUUM above (its heap has no string), and so is the new one of hm
CREATE FUNCTION hk_flags(idx regclass) RETURNS text LANGUAGE sql AS
$$ SELECT count(*) || ' segments, ' || count(*) FILTER (WHERE roots) || ' flagged' FROM sift._test_col_formats(idx) $$;
SELECT 'hk' AS idx, hk_flags('hk_col') UNION ALL SELECT 'ho', hk_flags('ho_col') UNION ALL SELECT 'hm', hk_flags('hm_col') UNION ALL SELECT 'hc', hk_flags('hc_col');

-- writes: new rows of every shape, rows that change their shape (an object becomes a string, a string an array, an array an object), deleted rows; in the write buffer
DO $$
DECLARE t text;
BEGIN
	FOREACH t IN ARRAY ARRAY['hk', 'ho', 'hm', 'hc'] LOOP
		EXECUTE format('INSERT INTO %I SELECT i, i %% 5, shape_nostr(i + 7) FROM generate_series(5001, 5240) i', t);
		EXECUTE format('INSERT INTO %I SELECT i, i %% 5, shape(i + 3) FROM generate_series(5241, 5480) i WHERE %L <> %L', t, t, 'hc');
		EXECUTE format('UPDATE %I SET doc = shape(id + 5) WHERE id %% 7 = 0 AND %L <> %L', t, t, 'hc');
		EXECUTE format('UPDATE %I SET doc = shape_nostr(id + 5) WHERE id %% 7 = 0 AND %L = %L', t, t, 'hc');
		EXECUTE format('DELETE FROM %I WHERE id %% 11 = 0', t);
	END LOOP;
END $$;
SELECT * FROM hk_run('hk');
SELECT * FROM hk_run('ho');
SELECT * FROM hk_run('hm');
SELECT * FROM hk_run('hc');

-- promoted, merged and vacuumed: the string roots of the heap stay (ho, hm keep their old segments unflagged), those of hc are flagged
SELECT sift.col_promote('hk_col'), sift.col_promote('ho_col'), sift.col_promote('hm_col'), sift.col_promote('hc_col');
VACUUM hk, ho, hm, hc;
SELECT 'hk' AS idx, hk_flags('hk_col') UNION ALL SELECT 'ho', hk_flags('ho_col') UNION ALL SELECT 'hm', hk_flags('hm_col') UNION ALL SELECT 'hc', hk_flags('hc_col');
SELECT * FROM hk_run('hk');
SELECT * FROM hk_run('ho');
SELECT * FROM hk_run('hm');
SELECT * FROM hk_run('hc');

-- the strings leave the heap: the next VACUUM flags what is left
DELETE FROM ho WHERE jsonb_typeof(doc) = 'string';
DELETE FROM hm WHERE jsonb_typeof(doc) = 'string';
VACUUM ho, hm;
SELECT 'ho' AS idx, hk_flags('ho_col') UNION ALL SELECT 'hm', hk_flags('hm_col');
SELECT * FROM hk_run('ho');
SELECT * FROM hk_run('hm');

DROP TABLE hk_q;
DROP FUNCTION hk_run(text), hk_check(text), hk_rows(text, bool), hk_flags(regclass);
DROP TABLE hk, ho, hm, hc;
DROP FUNCTION shape(int), shape_nostr(int);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
