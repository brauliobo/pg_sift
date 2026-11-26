-- A Format 2.0 sift_table keeps text, varchar, char(n), bytea and name as bare bytes and a uuid as its 16 bytes, as a sift_col index does (SIFT_COMPAT_NATIVE_STRINGS): their leaves are exact, so the
-- aggregate engine of Format 2.0 (SiftAggV6) reads them. Every check runs the same statement on the table and on a heap twin and compares the rows.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
CREATE EXTENSION IF NOT EXISTS pageinspect;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE ns (id int PRIMARY KEY, t text, v varchar(20), c char(8), n name, b bytea, u uuid, f4 float4, f8 float8, m numeric(12,2), w text, wb bytea) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE nh (LIKE ns INCLUDING ALL);

CREATE VIEW src AS SELECT g AS id,
	CASE WHEN g % 13 = 0 THEN NULL ELSE 'text ' || (g % 50) END AS t,
	CASE WHEN g % 17 = 0 THEN NULL ELSE 'var' || (g % 7) END AS v,
	CASE WHEN g % 19 = 0 THEN NULL ELSE 'c' || (g % 5) END AS c,
	CASE WHEN g % 23 = 0 THEN NULL ELSE ('name' || (g % 9))::name END AS n,
	CASE WHEN g % 29 = 0 THEN NULL ELSE decode(md5(g::text), 'hex') END AS b,
	CASE WHEN g % 31 = 0 THEN NULL ELSE ('00000000-0000-4000-8000-' || lpad(to_hex(g % 100), 12, '0'))::uuid END AS u,
	CASE WHEN g % 37 = 0 THEN NULL ELSE (g % 10)::float4 / 4 END AS f4,
	CASE WHEN g % 41 = 0 THEN 'NaN'::float8 ELSE (g % 17)::float8 / 8 END AS f8,
	CASE WHEN g % 43 = 0 THEN NULL ELSE (g % 300) / 7.0 END AS m,
	CASE WHEN g % 250 = 0 THEN repeat('w' || g, 3000) WHEN g % 250 = 1 THEN '' ELSE 'short' || g END AS w,
	CASE WHEN g % 250 = 0 THEN decode(repeat(md5(g::text), 400), 'hex') WHEN g % 250 = 1 THEN '\x'::bytea ELSE decode(md5(g::text), 'hex') END AS wb
FROM generate_series(1, 20000) g;
INSERT INTO ns SELECT * FROM src;
INSERT INTO nh SELECT * FROM src;
SELECT sift.table_maintain('ns') IS NOT NULL AS maintained;
ANALYZE ns;
ANALYZE nh;

CREATE FUNCTION same(name text, q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a int; b int;
BEGIN
	EXECUTE format('SELECT count(*) FROM ((%s) EXCEPT ALL (%s)) x', replace(q, '@T@', 'ns'), replace(q, '@T@', 'nh')) INTO a;
	EXECUTE format('SELECT count(*) FROM ((%s) EXCEPT ALL (%s)) x', replace(q, '@T@', 'nh'), replace(q, '@T@', 'ns')) INTO b;
	IF a + b > 0 THEN RAISE EXCEPTION 'FAILED %: % rows only in the table, % only in the heap', name, a, b; END IF;
	RETURN name || ': ok';
END $$;

CREATE FUNCTION twin(stmt text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE replace(stmt, '@T@', 'ns');
	EXECUTE replace(stmt, '@T@', 'nh');
END $$;

CREATE FUNCTION plan(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || q LOOP
		RETURN NEXT regexp_replace(line, '^\s+', '');
	END LOOP;
END $$;

-- the columns come back as they went in
SELECT same('every column', 'SELECT * FROM @T@');
SELECT same('lengths', 'SELECT id, length(t), length(v), length(c), length(n), octet_length(b), octet_length(wb), length(w) FROM @T@');
SELECT same('md5 of the wide values', 'SELECT id, md5(w), md5(wb) FROM @T@ WHERE id % 250 IN (0, 1)');
SELECT same('char(n) keeps its padding', 'SELECT id, c::text, octet_length(c) FROM @T@');
SELECT same('name keeps its 64 bytes', 'SELECT id, n, octet_length(n::text) FROM @T@');

-- filters, keys and aggregates on every type
SELECT same('text key', 'SELECT t, count(*), count(DISTINCT v), min(w), max(n) FROM @T@ GROUP BY t');
SELECT same('varchar key', 'SELECT v, count(*), count(DISTINCT t) FROM @T@ GROUP BY v');
SELECT same('char key', 'SELECT c, count(*), sum(m) FROM @T@ GROUP BY c');
SELECT same('name key', 'SELECT n, count(*), max(id) FROM @T@ GROUP BY n');
SELECT same('uuid key', 'SELECT u, count(*), min(id) FROM @T@ GROUP BY u');
SELECT same('bytea key', 'SELECT b, count(*) FROM @T@ WHERE id % 10 = 0 GROUP BY b');
SELECT same('numeric and float', 'SELECT f4, sum(m), sum(f8), max(f8), min(f4) FROM @T@ GROUP BY f4');
SELECT same('text equality', 'SELECT count(*), sum(id) FROM @T@ WHERE t = ''text 3''');
SELECT same('text in', 'SELECT v, count(*) FROM @T@ WHERE t IN (''text 1'', ''text 2'', ''text 40'') GROUP BY v');
SELECT same('text like', 'SELECT count(*), sum(id) FROM @T@ WHERE t LIKE ''text 1%''');
SELECT same('text not like', 'SELECT count(*) FROM @T@ WHERE t NOT LIKE ''%1%''');
SELECT same('text range', 'SELECT count(*) FROM @T@ WHERE t >= ''text 2'' AND t < ''text 4''');
SELECT same('varchar in', 'SELECT t, count(*) FROM @T@ WHERE v IN (''var1'', ''var2'') GROUP BY t');
SELECT same('char equality', 'SELECT count(*), sum(id) FROM @T@ WHERE c = ''c3''');
SELECT same('char in', 'SELECT count(*) FROM @T@ WHERE c IN (''c1'', ''c2'')');
SELECT same('name equality', 'SELECT count(*), sum(id) FROM @T@ WHERE n = ''name3''');
SELECT same('name range', 'SELECT count(*) FROM @T@ WHERE n > ''name3'' AND n <= ''name6''');
SELECT same('uuid equality', 'SELECT count(*), sum(id) FROM @T@ WHERE u = ''00000000-0000-4000-8000-000000000005''');
SELECT same('uuid in', 'SELECT count(*) FROM @T@ WHERE u IN (''00000000-0000-4000-8000-000000000005'', ''00000000-0000-4000-8000-00000000000a'')');
SELECT same('bytea equality', 'SELECT count(*) FROM @T@ WHERE b = decode(md5(''7''), ''hex'')');
SELECT same('numeric range', 'SELECT count(*), sum(m) FROM @T@ WHERE m > 10 AND m <= 30.5');
SELECT same('float range', 'SELECT count(*), sum(f8) FROM @T@ WHERE f8 < 1.5 AND f4 >= 0.5');
SELECT same('null tests', 'SELECT count(*) FROM @T@ WHERE t IS NULL OR n IS NULL OR u IS NOT NULL AND b IS NULL');
SELECT same('count distinct', 'SELECT count(DISTINCT t), count(DISTINCT n), count(DISTINCT u), count(DISTINCT b), count(DISTINCT c) FROM @T@');
SELECT same('top-n by name', 'SELECT id, n FROM (SELECT id, n FROM @T@ ORDER BY n, id LIMIT 7) x');
SELECT same('top-n by text desc', 'SELECT id, t FROM (SELECT id, t FROM @T@ ORDER BY t DESC NULLS LAST, id LIMIT 7) x');
SELECT same('top-n by uuid', 'SELECT id, u FROM (SELECT id, u FROM @T@ ORDER BY u, id LIMIT 7) x');

-- the leaves are exact
SELECT sift.table_maintain('ns') IS NOT NULL AS maintained_again;
SELECT plan('SELECT t, count(*) FROM ns GROUP BY t') AS text_key;
SELECT plan('SELECT v, count(*) FROM ns WHERE t = ''text 3'' GROUP BY v') AS text_filter;

-- an UPDATE of a few columns of a wide table keeps the address of the row and patches the columns (SIFT_PT_COL_DELTA)
CREATE FUNCTION delta_pages(rel regclass) RETURNS int LANGUAGE sql AS $$
	SELECT count(*)::int FROM generate_series(0, pg_relation_size(rel) / 8192 - 1) n
	WHERE get_byte(get_raw_page(rel::text, n::int), 8176) + 256 * get_byte(get_raw_page(rel::text, n::int), 8177) = 42 $$;

SELECT twin('UPDATE @T@ SET t = ''upd '' || id WHERE id % 3 = 0');
SELECT same('text column delta', 'SELECT * FROM @T@');
SELECT twin('UPDATE @T@ SET n = (''nm'' || (id % 5))::name WHERE id % 4 = 0');
SELECT same('name column delta', 'SELECT * FROM @T@');
SELECT twin('UPDATE @T@ SET u = NULL WHERE id % 6 = 0');
SELECT twin('UPDATE @T@ SET u = ''ffffffff-ffff-4fff-8fff-ffffffffffff'' WHERE id % 7 = 0');
SELECT same('uuid column delta', 'SELECT * FROM @T@');
SELECT twin('UPDATE @T@ SET b = ''\xdeadbeef'' WHERE id % 8 = 0');
SELECT twin('UPDATE @T@ SET v = ''v'' || id, c = ''x'' WHERE id % 9 = 0');
SELECT same('bytea, varchar and char column deltas', 'SELECT * FROM @T@');
SELECT twin('UPDATE @T@ SET w = repeat(''z'', 5000) WHERE id % 500 = 7');
SELECT twin('UPDATE @T@ SET t = NULL, n = NULL WHERE id % 11 = 0');
SELECT same('a value over the wide threshold and nulls', 'SELECT * FROM @T@');
SELECT delta_pages('ns') > 0 AS column_deltas_written;
SELECT same('text key after updates', 'SELECT t, count(*), count(DISTINCT n) FROM @T@ GROUP BY t');
SELECT same('text filter after updates', 'SELECT count(*), sum(id) FROM @T@ WHERE t LIKE ''upd 1%''');
SELECT same('top-n by name after updates', 'SELECT id, n FROM (SELECT id, n FROM @T@ ORDER BY n NULLS LAST, id LIMIT 7) x');
SELECT sift.table_maintain('ns') IS NOT NULL AS maintained_after_updates;
SELECT same('after maintenance', 'SELECT * FROM @T@');
SELECT same('text key after maintenance', 'SELECT t, v, count(*) FROM @T@ GROUP BY t, v');

SELECT twin('DELETE FROM @T@ WHERE id % 10 = 3');
SELECT same('after deletes', 'SELECT * FROM @T@');

-- a rewrite and the check
VACUUM FULL ns;
SELECT count(*) AS check_problems_after_vacuum_full FROM sift.check('ns'::regclass);
SELECT same('after VACUUM FULL', 'SELECT * FROM @T@');
SELECT same('text key after VACUUM FULL', 'SELECT t, count(*), count(DISTINCT n) FROM @T@ GROUP BY t');
INSERT INTO ns SELECT g, 'late ' || g, 'late', 'late', 'late', '\x00ff', gen_random_uuid(), 1, 2, 3, 'w', '\x01' FROM generate_series(20001, 20010) g;
SELECT count(*) AS check_problems_after_insert FROM sift.check('ns'::regclass);

-- a table of two or three columns
CREATE TABLE n2 (id int PRIMARY KEY, t text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE n3 (id int PRIMARY KEY, n name, u uuid) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO n2 SELECT g, 'row ' || g FROM generate_series(1, 5000) g;
INSERT INTO n3 SELECT g, ('nm' || g % 40)::name, ('00000000-0000-4000-8000-' || lpad(to_hex(g), 12, '0'))::uuid FROM generate_series(1, 5000) g;
UPDATE n2 SET t = 'upd ' || id WHERE id % 5 = 0;
UPDATE n3 SET n = 'changed' WHERE id % 5 = 0;
UPDATE n3 SET u = NULL WHERE id % 7 = 0;
SELECT count(*), count(DISTINCT t), min(t), max(t) FROM n2;
SELECT count(*), count(DISTINCT n), count(u), min(n), max(n) FROM n3;
SELECT count(*) AS n2_check_problems FROM sift.check('n2'::regclass);
SELECT count(*) AS n3_check_problems FROM sift.check('n3'::regclass);

DROP TABLE ns, nh, n2, n3;
DROP VIEW src;
DROP FUNCTION same(text, text), twin(text), plan(text), delta_pages(regclass);
DROP EXTENSION pageinspect;
DROP EXTENSION pg_sift;
