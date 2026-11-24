-- SEC_PREAGG version 2 on a sift_table and in the workers of a parallel aggregate, and the cubes a preagg option declares. Every answer is compared with a heap twin that has the same rows and is churned the same way; the
-- second word of a check says whether granules of the answer came from the cubes (preagg) or none did (scan). A table with preagg = 'off' is the same query without cubes.
CREATE EXTENSION IF NOT EXISTS pg_sift;
SET client_min_messages = warning;
SET jit = off;
SET timezone = 'UTC';
SET max_parallel_workers_per_gather = 0;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

CREATE FUNCTION answer(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT coalesce(string_agg(t::text, E''\n'' ORDER BY t::text), '''') FROM (' || q || ') t' INTO r;
	RETURN r;
END $$;

-- a counter of the line of EXPLAIN ANALYZE that has it
CREATE FUNCTION counter(q text, pat text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text;
		n int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP
		n := n + coalesce(substring(l from pat)::int, 0);
	END LOOP;
	RETURN n;
END $$;

-- q names the table @T: its answer on the table of the test and on its heap twin (@T || 'h')
CREATE FUNCTION chk(tbl text, q text) RETURNS text LANGUAGE sql AS $$
	SELECT CASE WHEN answer(replace(q, '@T', tbl)) = answer(replace(q, '@T', tbl || 'h')) THEN 'same' ELSE 'DIFF' END || ' ' ||
		   CASE WHEN counter(replace(q, '@T', tbl), 'preagg=(\d+)') > 0 THEN 'preagg' ELSE 'scan' END $$;

CREATE PROCEDURE apply(tbl text, sql text) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE replace(sql, '@T', tbl);
	EXECUTE replace(sql, '@T', tbl || 'h');
END $$;

CREATE PROCEDURE load(tbl text) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TABLE %I (id int PRIMARY KEY, g int, h int2, v int8, n numeric(12,2), f float8, pad text) USING sift_table WITH (autovacuum_enabled = false)', tbl);
	EXECUTE format('CREATE TABLE %I (id int PRIMARY KEY, g int, h int2, v int8, n numeric(12,2), f float8, pad text) USING heap WITH (autovacuum_enabled = false)', tbl || 'h');
END $$;

CREATE PROCEDURE fill(tbl text, lo int, hi int) LANGUAGE sql AS $$
	CALL apply(tbl, format('INSERT INTO @T SELECT i, i %% 7, CASE WHEN i %% 11 = 0 THEN NULL ELSE i %% 3 END, (i * 37) %% 10007, ((i * 31) %% 100000) / 100.0, (i %% 800) * 0.5,
		md5(i::text) || md5((i + 1)::text) || md5((i + 2)::text) FROM generate_series(%s, %s) i', lo, hi)) $$;

CREATE FUNCTION checks(tbl text) RETURNS TABLE (label text, result text) LANGUAGE sql AS $$
	VALUES ('global', chk(tbl, 'SELECT count(*), count(h), sum(v), sum(n), avg(n), min(v), max(v), sum(f), min(f), max(f) FROM @T')),
		   ('one key', chk(tbl, 'SELECT g, count(*), sum(v), min(n), max(n) FROM @T GROUP BY g')),
		   ('null key', chk(tbl, 'SELECT h, count(*), count(h), sum(v), avg(n) FROM @T GROUP BY h')),
		   ('two keys', chk(tbl, 'SELECT g, h, count(*), sum(v) FROM @T GROUP BY g, h')),
		   ('key test', chk(tbl, 'SELECT g, count(*), sum(v), sum(n) FROM @T WHERE g IN (1, 3, 5) GROUP BY g')),
		   ('null test', chk(tbl, 'SELECT g, count(*), sum(v) FROM @T WHERE h IS NULL GROUP BY g')),
		   ('zone test', chk(tbl, 'SELECT g, count(*), sum(v) FROM @T WHERE id > 0 GROUP BY g')),
		   ('value test', chk(tbl, 'SELECT g, count(*), sum(v) FROM @T WHERE v > 5000 GROUP BY g')) $$;

-- the cubes of a table the aggregate answers from: all that the table is written with
CALL load('tt');
CALL fill('tt', 1, 40000);
SELECT sift.table_maintain('tt') IS NOT NULL AS maintained;
VACUUM (FREEZE) tt, tth;
ANALYZE tt;
SELECT 'fresh' AS churn, * FROM checks('tt');

-- DELETE and UPDATE: a granule with a few dead rows is corrected from its cube, the others are answered whole, and the answers are the heap's
CALL apply('tt', 'DELETE FROM @T WHERE id % 250 = 7');
SELECT 'delete 0.4%' AS churn, * FROM checks('tt');
SELECT counter('SELECT g, count(*), sum(v) FROM tt GROUP BY g', 'preagg_delta=(\d+)') > 0 AS corrected_for_dead_rows;
VACUUM tt, tth;
SELECT 'vacuum' AS churn, * FROM checks('tt');
CALL apply('tt', 'DELETE FROM @T WHERE id BETWEEN 20001 AND 20400');
SELECT 'delete 400 consecutive rows' AS churn, * FROM checks('tt');
CALL apply('tt', 'UPDATE @T SET v = v + 1, f = f + 0.5 WHERE id % 500 = 3');
SELECT 'update' AS churn, * FROM checks('tt');
CALL apply('tt', 'UPDATE @T SET g = (g + 1) % 7 WHERE id % 400 = 9');
SELECT 'update a key' AS churn, * FROM checks('tt');
CALL fill('tt', 40001, 42000);
SELECT 'insert after' AS churn, * FROM checks('tt');
VACUUM tt, tth;
SELECT 'vacuum again' AS churn, * FROM checks('tt');

-- a transaction sees its own changes and no other's; ABORT leaves none
BEGIN;
CALL apply('tt', 'DELETE FROM @T WHERE id % 100 = 1');
CALL apply('tt', 'UPDATE @T SET v = 0 WHERE id % 100 = 2');
CALL fill('tt', 70001, 70500);
SELECT 'own changes' AS txn, * FROM checks('tt');
ROLLBACK;
SELECT 'rolled back' AS txn, * FROM checks('tt');
BEGIN;
CALL apply('tt', 'DELETE FROM @T WHERE id % 100 = 1');
COMMIT;
SELECT 'committed delete' AS txn, * FROM checks('tt');
VACUUM tt, tth;
SELECT 'vacuum' AS txn, * FROM checks('tt');

-- the cubes a table declares: 'off' stores none, a declaration stores the cubes it names (and the measures it names: the others scan)
CALL load('tf');
SELECT sift.table_set_option('tf', 'preagg', 'off') IS NOT NULL AS option_set;
CALL fill('tf', 1, 20000);
SELECT sift.table_maintain('tf') IS NOT NULL AS maintained;
VACUUM (FREEZE) tf, tfh;
SELECT 'off' AS preagg, label, result FROM checks('tf') WHERE label IN ('global', 'one key', 'two keys');

CALL load('te');
SELECT sift.table_set_option('te', 'preagg', 'cube(g: v, n; h)') IS NOT NULL AS option_set;
CALL fill('te', 1, 30000);
SELECT sift.table_maintain('te') IS NOT NULL AS maintained;
VACUUM (FREEZE) te, teh;
SELECT 'cube(g: v, n; h)' AS preagg, * FROM checks('te');
SELECT 'declared' AS preagg, chk('te', 'SELECT g, sum(v), sum(n), count(*) FROM @T GROUP BY g') AS dim_and_measures,
	   chk('te', 'SELECT h, sum(v), count(*) FROM @T GROUP BY h') AS second_dim,
	   chk('te', 'SELECT g, h, sum(n) FROM @T GROUP BY g, h') AS joint_cube,
	   chk('te', 'SELECT g, sum(f) FROM @T GROUP BY g') AS measure_not_declared,
	   chk('te', 'SELECT count(*), sum(v) FROM @T') AS no_key;
CALL apply('te', 'DELETE FROM @T WHERE id % 250 = 7');
SELECT 'declared, delete' AS preagg, chk('te', 'SELECT g, sum(v), sum(n), count(*) FROM @T GROUP BY g') AS dim_and_measures;
SELECT sift.table_set_option('te', 'preagg', 'cube(nosuchcolumn: v)');
SELECT sift.table_set_option('te', 'preagg', 'cube(g: v');
SELECT sift.table_set_option('te', 'preagg', 'cube(g: v, n; h); rollup(g, h); calendar(id)') IS NOT NULL AS clauses_read;

-- the same declaration on a sift_col index: stored for the parts, answered from them, and not for a measure it did not name
CREATE TABLE cc (id int PRIMARY KEY, g int, h int2, v int8, n numeric(12,2), f float8, pad text);
INSERT INTO cc SELECT i, i % 7, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 3 END, (i * 37) % 10007, ((i * 31) % 100000) / 100.0, (i % 800) * 0.5,
	md5(i::text) || md5((i + 1)::text) || md5((i + 2)::text) FROM generate_series(1, 30000) i;
CREATE TABLE cch (LIKE cc INCLUDING ALL);
INSERT INTO cch SELECT * FROM cc;
CREATE INDEX cc_col ON cc USING sift_col (id, g, h, v, n, f, pad) WITH (key_order = 'id', preagg = 'cube(g: v, n; h)');
VACUUM (FREEZE, ANALYZE) cc, cch;
SELECT chk('cc', 'SELECT g, sum(v), sum(n), count(*) FROM @T GROUP BY g') AS dim_and_measures,
	   chk('cc', 'SELECT h, sum(v), count(*) FROM @T GROUP BY h') AS second_dim,
	   chk('cc', 'SELECT g, h, sum(n) FROM @T GROUP BY g, h') AS joint_cube,
	   chk('cc', 'SELECT g, sum(f) FROM @T GROUP BY g') AS measure_not_declared;
DELETE FROM cc WHERE id % 250 = 7;
DELETE FROM cch WHERE id % 250 = 7;
SELECT chk('cc', 'SELECT g, sum(v), sum(n), count(*) FROM @T GROUP BY g') AS declared_dirty_pages;
VACUUM cc, cch;
SELECT chk('cc', 'SELECT g, sum(v), sum(n), count(*) FROM @T GROUP BY g') AS declared_after_vacuum;

-- the workers of a parallel aggregate answer granules from the cubes, correct them for dead rows, and scan the rest: the answer is the heap's, workers were launched, granules came from the cubes
CREATE TABLE pc (id int PRIMARY KEY, g int, h int2, v int8, n numeric(12,2), f float8, pad text);
INSERT INTO pc SELECT i, i % 7, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 3 END, (i * 37) % 10007, ((i * 31) % 100000) / 100.0, (i % 800) * 0.5,
	md5(i::text) FROM generate_series(1, 40000) i;
CREATE TABLE pch (LIKE pc INCLUDING ALL);
INSERT INTO pch SELECT * FROM pc;
CREATE INDEX pc_col ON pc USING sift_col (id, g, h, v, n, f, pad) WITH (key_order = 'id');
VACUUM (FREEZE, ANALYZE) pc, pch;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
CREATE FUNCTION par(q text) RETURNS text LANGUAGE sql AS $$
	SELECT CASE WHEN answer(replace(q, '@T', 'pc')) = answer(replace(q, '@T', 'pch')) THEN 'same' ELSE 'DIFF' END || ' ' ||
		   CASE WHEN counter(replace(q, '@T', 'pc'), 'Workers Launched: (\d+)') > 0 THEN 'workers' ELSE 'serial' END || ' ' ||
		   CASE WHEN counter(replace(q, '@T', 'pc'), 'preagg=(\d+)') > 0 THEN 'preagg' ELSE 'scan' END $$;
CREATE FUNCTION pars() RETURNS TABLE (label text, result text) LANGUAGE sql AS $$
	VALUES ('grouped', par('SELECT g, count(*), sum(v), sum(n), min(v), max(v) FROM @T GROUP BY g')),
		   ('global', par('SELECT count(*), sum(v), sum(n) FROM @T')),
		   ('two keys', par('SELECT g, h, sum(n) FROM @T GROUP BY g, h')),
		   ('distinct scans', par('SELECT g, count(DISTINCT h) FROM @T GROUP BY g')) $$;
SELECT 'fresh' AS churn, * FROM pars();
DELETE FROM pc WHERE id % 400 = 5;
DELETE FROM pch WHERE id % 400 = 5;
SELECT 'delete, pages not yet vacuumed' AS churn, * FROM pars();
VACUUM pc, pch;
SELECT 'delete, vacuumed' AS churn, * FROM pars();
SELECT counter('SELECT g, count(*), sum(v) FROM pc GROUP BY g', 'preagg_delta=(\d+)') > 0 AS corrected_for_dead_rows;
INSERT INTO pc SELECT i, i % 7, i % 3, (i * 37) % 10007, (i % 1000) / 100.0, 0, 'x' FROM generate_series(40001, 41500) i;
INSERT INTO pch SELECT i, i % 7, i % 3, (i * 37) % 10007, (i % 1000) / 100.0, 0, 'x' FROM generate_series(40001, 41500) i;
SELECT 'insert, in the slab' AS churn, * FROM pars();
UPDATE pc SET v = v + 1 WHERE id % 1000 = 3;
UPDATE pch SET v = v + 1 WHERE id % 1000 = 3;
VACUUM pc, pch;
SELECT 'update, vacuumed' AS churn, * FROM pars();

DROP TABLE tt, tth, tf, tfh, te, teh, cc, cch, pc, pch;
DROP PROCEDURE load(text), fill(text, int, int), apply(text, text);
DROP FUNCTION pars(), par(text), checks(text), chk(text, text), counter(text, text), answer(text);
DROP EXTENSION pg_sift;
