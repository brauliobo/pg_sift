-- SiftAggV6 reads uuid, bytea, char(n), name and numeric columns of a sift_table as group keys, COUNT(DISTINCT) arguments, quals and aggregate arguments, with PostgreSQL's equality (the datum
-- decode of the store gives it the values as PostgreSQL holds them). Every check runs the statement on the table and on a heap twin holding the same rows and compares the rows as text (so the
-- padding of a char(n) and the display scale of a numeric(30,5) are compared too); the plan of the table is asserted to be SiftAggV6.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET cpu_tuple_cost = 100;
SET random_page_cost = 10000;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
SET enable_bitmapscan = off;

CREATE TABLE wt (id int PRIMARY KEY, g int, u uuid, b bytea, c char(6), n name, nm numeric, m30 numeric(30,5), m12 numeric(12,2)) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE wh (LIKE wt INCLUDING ALL);

CREATE VIEW bulk AS SELECT g AS id, g % 5 AS g,
	CASE WHEN g % 31 = 0 THEN NULL ELSE ('00000000-0000-4000-8000-' || lpad(to_hex(g % 60), 12, '0'))::uuid END AS u,
	CASE WHEN g % 29 = 0 THEN NULL ELSE decode(lpad(to_hex(g % 40), 2, '0') || CASE WHEN g % 3 = 0 THEN '00' WHEN g % 3 = 1 THEN '20' ELSE '' END, 'hex') END AS b,
	CASE WHEN g % 19 = 0 THEN NULL ELSE ('c' || (g % 7)) || repeat(' ', g % 3) END AS c,
	CASE WHEN g % 23 = 0 THEN NULL ELSE ('name' || (g % 9))::name END AS n,
	CASE WHEN g % 17 = 0 THEN NULL ELSE ((g % 11)::numeric / 4) * (CASE WHEN g % 2 = 0 THEN 1.0 ELSE 1.00 END) END AS nm,
	CASE WHEN g % 17 = 0 THEN NULL ELSE ((g % 13)::numeric / 8) END AS m30,
	CASE WHEN g % 43 = 0 THEN NULL ELSE ((g % 300)::numeric / 7) END AS m12
FROM generate_series(1, 6000) g;

CREATE VIEW edge AS
SELECT 100000 + i AS id, 9 AS g, u, b, c, n, nm, m30, 1.5::numeric(12,2) AS m12
FROM (VALUES
	(1, '00000000-0000-0000-0000-000000000000'::uuid, '\x'::bytea, ''::char(6), ''::name, '1.0'::numeric, 1.5::numeric(30,5)),
	(2, 'ffffffff-ffff-ffff-ffff-ffffffffffff', '\x00', '      ', 'a', '1.00', 1.50),
	(3, '00000000-0000-0000-0000-000000000001', '\x20', 'a', 'a ', '1.000', 1.500),
	(4, '00000000-0000-0000-0000-000000000100', '\x2000', 'a ', ' a', '0', 0),
	(5, '00000000-0000-0000-0100-000000000000', '\x0020', 'a    ', 'a  ', '0.0', 0.00001),
	(6, '01000000-0000-0000-0000-000000000000', '\x00000000', 'a b', 'nameé', '-0', 123456789012345678901234.5),
	(7, '00000000-0000-0000-0000-0000000000ff', '\x0000000000000000000000000000', ' a', repeat('x', 63), '-0.00', 123456789012345678901234.50),
	(8, NULL, NULL, NULL, NULL, 'NaN', NULL),
	(9, '00000000-0000-0000-0000-000000000000', '\xff', 'é', 'N', 'NaN', 'NaN'),
	(10, 'ffffffff-ffff-ffff-ffff-fffffffffffe', '\xffff', 'é  ', 'n ', 'Infinity', NULL),
	(11, 'ffffffff-ffff-ffff-ffff-fffffffffffe', '\xfffe', 'abcdef', 'n', '-Infinity', NULL),
	(12, '00000000-0000-0000-0000-000000000002', '\x20202020202020202020202020202020', 'abcde', 'n' || chr(9), '1234567890123456789012345678901234567890.123456789012345678901234567890', -1.5),
	(13, '00000000-0000-0000-0000-000000000003', '\x01', 'a' || chr(1), 'n1', '1234567890123456789012345678901234567890.12345678901234567890123456789', -1.50),
	(14, '00000000-0000-0000-0000-000000000004', '\x0102030405060708090a0b0c0d0e0f10', 'a' || chr(9), 'n10', '0.0000000000000000000000000001', 99999999999999999999999.99999),
	(15, '00000000-0000-0000-0000-000000000005', '\x0102030405060708090a0b0c0d0e0f11', 'a b ', 'n1 ', '1E+30', -99999999999999999999999.99999),
	(16, '00000000-0000-0000-0000-000000000005', '\x0102030405060708090a0b0c0d0e0f11', 'a b', 'n1', '1000000000000000000000000000000', 0.00000),
	(17, '00000000-0000-0000-0000-000000000006', '\x0102030405060708090a0b0c0d0e0f12', 'c1', 'name1', '100', 100),
	(18, '00000000-0000-0000-0000-000000000006', '\x0102030405060708090a0b0c0d0e0f12', 'c1 ', 'name1', '100.0', 100.0),
	(19, NULL, NULL, NULL, NULL, '1.5', NULL),
	(20, NULL, NULL, NULL, NULL, '1.50', NULL)
) AS v(i, u, b, c, n, nm, m30)
UNION ALL
SELECT 100100, 9, NULL, NULL, NULL, NULL, repeat('9', 5000)::numeric, NULL, NULL
UNION ALL
SELECT 100101, 9, NULL, NULL, NULL, NULL, (repeat('9', 5000) || '.0')::numeric, NULL, NULL
UNION ALL
SELECT 100102, 9, NULL, NULL, NULL, NULL, (repeat('9', 5000) || '.00')::numeric, NULL, NULL;

INSERT INTO wt SELECT * FROM bulk UNION ALL SELECT * FROM edge;
INSERT INTO wh SELECT * FROM bulk UNION ALL SELECT * FROM edge;
SELECT sift.table_maintain('wt') IS NOT NULL AS maintained;
ANALYZE wt;
ANALYZE wh;

CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; plan text := '';
BEGIN
	FOR r IN EXECUTE 'EXPLAIN (COSTS off) ' || replace(q, '@T@', 'wt') LOOP plan := plan || r; END LOOP;
	RETURN plan;
END $$;

-- v6: expect SiftAggV6 for the table (true), or another plan (false; NULL: either); the rows of the table and of the heap twin are the same either way
CREATE FUNCTION same(name text, q text, v6 boolean, as_text boolean DEFAULT true) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a int; b int; t text; h text; used boolean;
BEGIN
	t := replace(q, '@T@', 'wt');
	h := replace(q, '@T@', 'wh');
	IF as_text THEN
		EXECUTE format('SELECT count(*) FROM ((SELECT x::text FROM (%s) x) EXCEPT ALL (SELECT x::text FROM (%s) x)) y', t, h) INTO a;
		EXECUTE format('SELECT count(*) FROM ((SELECT x::text FROM (%s) x) EXCEPT ALL (SELECT x::text FROM (%s) x)) y', h, t) INTO b;
	ELSE
		EXECUTE format('SELECT count(*) FROM ((%s) EXCEPT ALL (%s)) y', t, h) INTO a;
		EXECUTE format('SELECT count(*) FROM ((%s) EXCEPT ALL (%s)) y', h, t) INTO b;
	END IF;
	IF a + b > 0 THEN RAISE EXCEPTION 'FAILED %: % rows only in the table, % only in the heap', name, a, b; END IF;
	used := plan_of(q) LIKE '%SiftAggV6%';
	IF used IS DISTINCT FROM v6 AND v6 IS NOT NULL THEN RAISE EXCEPTION 'FAILED %: SiftAggV6 %', name, CASE WHEN used THEN 'chosen' ELSE 'not chosen' END; END IF;
	RETURN name || ': ok';
END $$;

\echo == uuid
SELECT same('u group', 'SELECT u, count(*) FROM @T@ GROUP BY u', true);
SELECT same('u group x g', 'SELECT g, u, count(*), count(b) FROM @T@ GROUP BY g, u', true);
SELECT same('u distinct', 'SELECT g, count(DISTINCT u), count(u) FROM @T@ GROUP BY g', true);
SELECT same('u distinct all', 'SELECT count(DISTINCT u), count(u), count(*) FROM @T@', true);
SELECT same('u =', 'SELECT g, count(*) FROM @T@ WHERE u = ''00000000-0000-0000-0000-000000000000'' GROUP BY g', true);
SELECT same('u in', 'SELECT g, count(*) FROM @T@ WHERE u IN (''00000000-0000-0000-0000-000000000000'', ''ffffffff-ffff-ffff-ffff-fffffffffffe'', ''00000000-0000-4000-8000-000000000007'') GROUP BY g', true);
SELECT same('u is null', 'SELECT g, count(*) FROM @T@ WHERE u IS NULL GROUP BY g', true);
SELECT same('u is not null', 'SELECT g, count(*) FROM @T@ WHERE u IS NOT NULL GROUP BY g', true);
SELECT same('u range', 'SELECT g, count(*) FROM @T@ WHERE u < ''00000000-0000-4000-8000-000000000010'' AND u >= ''00000000-0000-0000-0000-000000000001'' GROUP BY g', true);
SELECT same('u key x u qual', 'SELECT u, count(*) FROM @T@ WHERE u > ''00000000-0000-4000-8000-000000000020'' GROUP BY u', true);

\echo == bytea
SELECT same('b group', 'SELECT b, count(*) FROM @T@ GROUP BY b', true);
SELECT same('b group x g', 'SELECT g, b, count(*) FROM @T@ GROUP BY g, b', true);
SELECT same('b distinct', 'SELECT g, count(DISTINCT b) FROM @T@ GROUP BY g', true);
SELECT same('b distinct all', 'SELECT count(DISTINCT b), count(b) FROM @T@', true);
SELECT same('b = empty', 'SELECT g, count(*) FROM @T@ WHERE b = ''\x'' GROUP BY g', true);
SELECT same('b = 00', 'SELECT g, count(*) FROM @T@ WHERE b = ''\x00'' GROUP BY g', true);
SELECT same('b = 20', 'SELECT g, count(*) FROM @T@ WHERE b = ''\x20'' GROUP BY g', true);
SELECT same('b in', 'SELECT g, count(*) FROM @T@ WHERE b IN (''\x'', ''\x00'', ''\x2000'', ''\x0020'', ''\x1f'') GROUP BY g', true);
SELECT same('b is null', 'SELECT g, count(*) FROM @T@ WHERE b IS NULL GROUP BY g', true);
SELECT same('b range', 'SELECT g, count(*) FROM @T@ WHERE b > ''\x00'' AND b <= ''\x0f'' GROUP BY g', true);
SELECT same('b range empty', 'SELECT g, count(*) FROM @T@ WHERE b >= ''\x'' GROUP BY g', true);

\echo == char(n)
SELECT same('c group', 'SELECT c, count(*) FROM @T@ GROUP BY c', true);
SELECT same('c group x g', 'SELECT g, c, count(*) FROM @T@ GROUP BY g, c', true);
SELECT same('c distinct', 'SELECT g, count(DISTINCT c) FROM @T@ GROUP BY g', true);
SELECT same('c distinct all', 'SELECT count(DISTINCT c), count(c) FROM @T@', true);
SELECT same('c =', 'SELECT g, count(*) FROM @T@ WHERE c = ''a'' GROUP BY g', true);
SELECT same('c = padded', 'SELECT g, count(*) FROM @T@ WHERE c = ''a    '' GROUP BY g', true);
SELECT same('c = typed', 'SELECT g, count(*) FROM @T@ WHERE c = ''c1''::char(2) GROUP BY g', true);
SELECT same('c = empty', 'SELECT g, count(*) FROM @T@ WHERE c = '''' GROUP BY g', true);
SELECT same('c = multibyte', 'SELECT g, count(*) FROM @T@ WHERE c = ''é'' GROUP BY g', true);
SELECT same('c = too long', 'SELECT g, count(*) FROM @T@ WHERE c = ''abcdefgh'' GROUP BY g', true);
SELECT same('c = control', 'SELECT g, count(*) FROM @T@ WHERE c = (''a'' || chr(1))::char(6) GROUP BY g', true);
SELECT same('c in', 'SELECT g, count(*) FROM @T@ WHERE c IN (''a'', ''c1 '', ''é  '', ''abcdef'') GROUP BY g', true);
SELECT same('c is null', 'SELECT g, count(*) FROM @T@ WHERE c IS NULL GROUP BY g', true);
SELECT same('c range', 'SELECT g, count(*) FROM @T@ WHERE c < ''c3'' GROUP BY g', true);
SELECT same('c range control', 'SELECT g, count(*) FROM @T@ WHERE c >= (''a'' || chr(1))::char(6) GROUP BY g', true);

\echo == name
SELECT same('n group', 'SELECT n, count(*) FROM @T@ GROUP BY n', true);
SELECT same('n group x g', 'SELECT g, n, count(*) FROM @T@ GROUP BY g, n', true);
SELECT same('n distinct', 'SELECT g, count(DISTINCT n) FROM @T@ GROUP BY g', true);
SELECT same('n distinct all', 'SELECT count(DISTINCT n), count(n) FROM @T@', true);
SELECT same('n =', 'SELECT g, count(*) FROM @T@ WHERE n = ''name3'' GROUP BY g', true);
SELECT same('n = trailing blank', 'SELECT g, count(*) FROM @T@ WHERE n = ''n '' GROUP BY g', true);
SELECT same('n in', 'SELECT g, count(*) FROM @T@ WHERE n IN (''name1'', ''n'', ''nameé'', ''N'') GROUP BY g', true);
SELECT same('n is null', 'SELECT g, count(*) FROM @T@ WHERE n IS NULL GROUP BY g', true);
SELECT same('n long', 'SELECT g, count(*) FROM @T@ WHERE n = repeat(''x'', 63)::name GROUP BY g', true);
SELECT same('n range', 'SELECT g, count(*) FROM @T@ WHERE n < ''name5'' GROUP BY g', true);

\echo == numeric (no scaled integer leaf)
-- a group of numeric_cmp-equal values (1.0, 1.00, 1.000) is one group, NaN equals NaN, +-Infinity are values; compared by value
SELECT same('nm group', 'SELECT nm, count(*) FROM @T@ GROUP BY nm', true, false);
SELECT same('nm group x g', 'SELECT g, nm, count(*), count(nm) FROM @T@ GROUP BY g, nm', true, false);
SELECT same('nm distinct', 'SELECT g, count(DISTINCT nm), count(nm) FROM @T@ GROUP BY g', true, false);
SELECT same('nm distinct all', 'SELECT count(DISTINCT nm), count(nm) FROM @T@', true, false);
SELECT same('nm is null', 'SELECT g, count(*) FROM @T@ WHERE nm IS NULL GROUP BY g', true, false);
SELECT same('nm is not null', 'SELECT g, count(*) FROM @T@ WHERE nm IS NOT NULL GROUP BY g', true, false);
-- a qual on a numeric image is decided by PostgreSQL (the stored image keeps the display scale), and so are sum, avg, min and max
SELECT same('nm =', 'SELECT g, count(*) FROM @T@ WHERE nm = 1 GROUP BY g', true, false);
SELECT same('nm = NaN', 'SELECT g, count(*) FROM @T@ WHERE nm = ''NaN'' GROUP BY g', true, false);
SELECT same('nm in', 'SELECT g, count(*) FROM @T@ WHERE nm IN (1.0, 100, ''Infinity'') GROUP BY g', true, false);
SELECT same('nm range', 'SELECT g, count(*) FROM @T@ WHERE nm > 2 GROUP BY g', true, false);
SELECT same('nm sum', 'SELECT g, sum(nm) FROM @T@ WHERE nm < ''Infinity'' GROUP BY g', true, false);
SELECT same('nm max', 'SELECT g, max(nm) FROM @T@ GROUP BY g', true, false);
-- numeric(30,5): every value has the display scale of the typmod, which the group returns as it was stored
SELECT same('m30 group', 'SELECT m30, count(*) FROM @T@ GROUP BY m30', true);
SELECT same('m30 group x g', 'SELECT g, m30, count(*) FROM @T@ GROUP BY g, m30', true);
SELECT same('m30 distinct', 'SELECT g, count(DISTINCT m30) FROM @T@ GROUP BY g', true);
SELECT same('m30 is null', 'SELECT g, count(*) FROM @T@ WHERE m30 IS NULL GROUP BY g', true);
-- a numeric(12,2) keeps its scaled integer leaf: its sum is read as before, next to a numeric image key
SELECT same('m12 sum by nm', 'SELECT nm, sum(m12), count(*) FROM @T@ GROUP BY nm', true, false);

\echo == the display scale of a numeric with no typmod: the table returns the least scale that holds the value, the heap the scale of the first row of the group
SELECT nm::text AS nm_table, count(*) FROM wt WHERE id IN (100001, 100002, 100003, 100019, 100020) GROUP BY nm ORDER BY nm;
SELECT nm::text AS nm_heap, count(*) FROM wh WHERE id IN (100001, 100002, 100003, 100019, 100020) GROUP BY nm ORDER BY nm;
SELECT length(nm::text) AS wide_digits, count(*) FROM wt WHERE id >= 100100 GROUP BY nm;

\echo == FILTER, several wide keys, many groups and the aggregates the engine leaves to PostgreSQL
SELECT same('filter is null', 'SELECT g, count(*) FILTER (WHERE u IS NULL), count(*) FILTER (WHERE nm IS NULL), count(*) FILTER (WHERE m30 IS NOT NULL), count(nm) FROM @T@ GROUP BY g', true, false);
SELECT same('multi key', 'SELECT u, b, c, n, nm, m30, count(*) FROM @T@ GROUP BY u, b, c, n, nm, m30', true, false);
SELECT same('multi key distinct', 'SELECT c, count(DISTINCT n), count(DISTINCT b), count(DISTINCT m30) FROM @T@ GROUP BY c', true, false);
SELECT same('rollup', 'SELECT g, c, count(*) FROM @T@ GROUP BY ROLLUP (g, c)', true);
SELECT same('having', 'SELECT nm, count(*) FROM @T@ GROUP BY nm HAVING count(*) > 5', true, false);
SELECT same('min max', 'SELECT g, min(c), max(n), min(u::text) FROM @T@ GROUP BY g', true, false);
SELECT same('numeric image key, sum of m12', 'SELECT nm, sum(m12), count(m12) FROM @T@ GROUP BY nm', true, false);
SET work_mem = '64kB';
SELECT same('many groups', 'SELECT u, b, nm, g, count(*) FROM @T@ GROUP BY u, b, nm, g', true, false);
RESET work_mem;

-- ORDER BY a name with a LIMIT: the Top-N operators read integer keys only, so the rows come from PostgreSQL's plan
SELECT same('n order limit', 'SELECT n, id FROM @T@ ORDER BY n, id LIMIT 7', NULL);
SELECT same('u order limit', 'SELECT u, id FROM @T@ ORDER BY u, id LIMIT 7', NULL);

\echo == after updates, deletes and inserts: the rows staged, then the table maintained
CREATE FUNCTION on_both(q text) RETURNS void LANGUAGE plpgsql AS $$ BEGIN EXECUTE replace(q, '@T@', 'wt'); EXECUTE replace(q, '@T@', 'wh'); END $$;
CREATE FUNCTION mutated(phase text) RETURNS SETOF text LANGUAGE sql AS $$
	SELECT same(phase || ' u', 'SELECT u, count(*) FROM @T@ GROUP BY u', true)
	UNION ALL SELECT same(phase || ' b', 'SELECT b, count(*) FROM @T@ GROUP BY b', true)
	UNION ALL SELECT same(phase || ' c', 'SELECT c, count(*) FROM @T@ GROUP BY c', true)
	UNION ALL SELECT same(phase || ' n', 'SELECT n, count(*) FROM @T@ GROUP BY n', true)
	UNION ALL SELECT same(phase || ' nm', 'SELECT nm, count(*) FROM @T@ GROUP BY nm', true, false)
	UNION ALL SELECT same(phase || ' m30', 'SELECT m30, count(*) FROM @T@ GROUP BY m30', true)
	UNION ALL SELECT same(phase || ' c =', 'SELECT g, count(*) FROM @T@ WHERE c = ''zz'' GROUP BY g', true)
	UNION ALL SELECT same(phase || ' u is null', 'SELECT g, count(*) FROM @T@ WHERE u IS NULL GROUP BY g', true)
	UNION ALL SELECT same(phase || ' distinct', 'SELECT g, count(DISTINCT u), count(DISTINCT nm), count(DISTINCT c) FROM @T@ GROUP BY g', true, false)
$$;
SELECT on_both('UPDATE @T@ SET c = ''zz'' WHERE id % 97 = 0');
SELECT on_both('UPDATE @T@ SET nm = nm + 0.5, u = NULL, n = ''upd'' WHERE id % 89 = 0');
SELECT on_both('UPDATE @T@ SET b = ''\x0000'' WHERE id % 83 = 0');
SELECT on_both('DELETE FROM @T@ WHERE id % 53 = 0');
SELECT on_both('INSERT INTO @T@ SELECT id + 200000, g, u, b, c, n, nm, m30, m12 FROM bulk WHERE id % 7 = 0');
SELECT * FROM mutated('staged');
SELECT sift.table_maintain('wt') IS NOT NULL AS maintained;
SELECT * FROM mutated('maintained');

\echo == the plan reads the table itself, which has no index
SELECT plan_of('SELECT u, count(*) FROM @T@ GROUP BY u') LIKE '%Table: wt%' AS reads_table;

DROP TABLE wt, wh;
DROP VIEW bulk, edge;
DROP FUNCTION plan_of(text), same(text, text, boolean, boolean), on_both(text), mutated(text);
DROP EXTENSION pg_sift;
