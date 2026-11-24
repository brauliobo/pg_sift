-- The dimension of a star join of SiftAggV6 is kept for the next statement when its table keeps a key journal (it is the parent of a connection): EXPLAIN ANALYZE says "Dimension Cache: N hit, M built"
-- and the transaction's count of scans of the dimension shows that a hit reads nothing. Every write of the table, committed or not, and every DDL makes the next statement read the table again; every answer
-- is compared with the heap's (sift.enable_agg = off). A table under row level security, a table of another user and a table that keeps no journal are never served from another statement's read.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET enable_hashagg = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET client_min_messages = warning;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

CREATE TABLE dim (k int PRIMARY KEY, a int, b int, flag bool) WITH (autovacuum_enabled = false);
INSERT INTO dim SELECT i, i % 7, i % 3, i % 5 = 0 FROM generate_series(0, 699) i;
CREATE TABLE plain (k2 int PRIMARY KEY, c int) WITH (autovacuum_enabled = false);
INSERT INTO plain SELECT i, i % 4 FROM generate_series(0, 99) i;
CREATE TABLE ev (id int PRIMARY KEY, k int, k2 int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, CASE WHEN i % 211 = 0 THEN NULL ELSE i % 750 END, i % 120, i % 11 FROM generate_series(1, 3000) i;
CREATE INDEX dim_col ON dim USING sift_col (k, a, b, flag);
CREATE INDEX ev_col ON ev USING sift_col (id, k, k2, v) WITH (connect = 'k -> dim(k)');
VACUUM (FREEZE, ANALYZE) ev, dim, plain;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT dim.a, count(*), sum(ev.v) FROM ev JOIN dim ON dim.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (2, $q$SELECT dim.a, plain.c, count(dim.flag), sum(ev.v) FROM ev JOIN plain ON plain.k2 = ev.k2 LEFT JOIN dim ON dim.k = ev.k AND dim.flag GROUP BY 1, 2 ORDER BY 1, 2$q$);

CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;
CREATE FUNCTION cache(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || sql LOOP
		IF l LIKE '%Dimension Cache:%' THEN RETURN trim(split_part(l, ':', 2)); END IF;
	END LOOP;
	RETURN 'none';
END $$;
CREATE FUNCTION engaged(sql text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l LIKE '%SiftAggV6%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
-- the first statement after a change says what it did with the dimensions, the second what the next one does
CREATE FUNCTION report(label text, upto int DEFAULT 1) RETURNS TABLE (stage text, shape int, first_statement text, same_answers bool, next_statement text) LANGUAGE plpgsql AS $$
DECLARE s record; f text; ok bool; BEGIN
	FOR s IN SELECT * FROM shapes WHERE n <= upto ORDER BY n LOOP
		f := cache(s.sql);
		ok := answer(s.sql, true) IS NOT DISTINCT FROM answer(s.sql, false);
		RETURN QUERY SELECT label, s.n, f, ok, cache(s.sql);
	END LOOP;
END $$;
CREATE FUNCTION scans(t regclass) RETURNS bigint LANGUAGE sql AS $$ SELECT coalesce(pg_stat_get_xact_numscans(t), 0) $$;
-- the first statement after a write reads the dimension and keeps it, the next one is served from it: it scans the table that keeps no journal again and the dimension not
SELECT n, engaged(sql) AS engine FROM shapes ORDER BY n;
SELECT * FROM report('built', 2);
UPDATE dim SET b = b WHERE k = 1;
BEGIN;
SELECT scans('dim') AS dim0, scans('plain') AS plain0 \gset
SELECT answer((SELECT sql FROM shapes WHERE n = 2), true) IS NOT NULL AS first_statement;
SELECT scans('dim') - :dim0 AS dimension_scans, scans('plain') - :plain0 AS plain_scans;
SELECT answer((SELECT sql FROM shapes WHERE n = 2), true) IS NOT NULL AS next_statement;
SELECT scans('dim') - :dim0 AS dimension_scans, scans('plain') - :plain0 AS plain_scans;
COMMIT;

-- committed writes of the table: an update of the payload, of the key, a delete, an insert
UPDATE dim SET a = 6, flag = NULL WHERE k % 31 = 0;
SELECT * FROM report('payload update');
UPDATE dim SET k = k + 100000 WHERE k % 97 = 0;
SELECT * FROM report('key update');
DELETE FROM dim WHERE k % 53 = 0;
SELECT * FROM report('delete');
INSERT INTO dim SELECT i, 1, 1, true FROM generate_series(700, 749) i;
SELECT * FROM report('insert');
-- a table that keeps no journal is read by every statement, and its changes are seen at once
UPDATE plain SET c = 9 WHERE k2 % 7 = 0;
SELECT * FROM report('table without a journal', 2);

-- the writer's own transaction (the statement asks the journal, which writes the records the transaction holds): it reads the table it changed and keeps nothing of it; a rollback leaves the kept table as it was
BEGIN;
UPDATE dim SET b = 2 WHERE k % 17 = 0;
SELECT * FROM report('in the transaction', 2);
ROLLBACK;
SELECT * FROM report('after the abort');
BEGIN;
SAVEPOINT s;
UPDATE dim SET b = 2 WHERE k % 19 = 0;
INSERT INTO dim VALUES (5000, 0, 0, false);
ROLLBACK TO s;
SELECT * FROM report('after the savepoint');
COMMIT;
-- DDL
UPDATE dim SET a = a + 1;
SELECT * FROM report('journal wrapped');
ALTER TABLE dim ADD COLUMN extra int DEFAULT 5;
SELECT * FROM report('add column');
ALTER TABLE dim DROP COLUMN extra;
REINDEX INDEX dim_col;
SELECT * FROM report('reindex');
TRUNCATE dim;
SELECT * FROM report('truncate');
INSERT INTO dim SELECT i, i % 7, i % 3, i % 5 = 0 FROM generate_series(0, 299) i;
ALTER INDEX ev_col SET (connect = 'off');
DROP INDEX dim_col;
SELECT * FROM report('journal dropped');
DROP TABLE dim CASCADE;
CREATE TABLE dim (k int PRIMARY KEY, a int, b int, flag bool) WITH (autovacuum_enabled = false);
INSERT INTO dim SELECT i, i % 5, i % 2, i % 3 = 0 FROM generate_series(0, 799) i;
CREATE INDEX dim_col ON dim USING sift_col (k, a, b, flag);
CREATE INDEX ev_col2 ON ev USING sift_col (id, k, k2, v) WITH (connect = 'k -> dim(k)');
SELECT * FROM report('table dropped and made again');

-- users: a kept table is of the user that read it; a table under row level security is read by every statement
CREATE ROLE dcache_a;
CREATE ROLE dcache_b;
GRANT SELECT ON dim, plain, ev, shapes TO dcache_a, dcache_b;
SET ROLE dcache_a;
SELECT * FROM report('user a');
SET ROLE dcache_b;
SELECT * FROM report('user b');
RESET ROLE;

CREATE TABLE rdim (k int PRIMARY KEY, who text, a int) WITH (autovacuum_enabled = false);
INSERT INTO rdim SELECT i, CASE WHEN i % 2 = 0 THEN 'dcache_a' ELSE 'dcache_b' END, i % 5 FROM generate_series(0, 749) i;
CREATE INDEX rdim_col ON rdim USING sift_col (k, who, a);
CREATE INDEX ev_rls ON ev USING sift_col (id, k, k2, v) WITH (connect = 'k -> rdim(k)');
GRANT SELECT ON rdim TO dcache_a, dcache_b;
DELETE FROM shapes;
INSERT INTO shapes VALUES
 (1, $q$SELECT rdim.who, count(*), sum(ev.v) FROM ev JOIN rdim ON rdim.k = ev.k GROUP BY 1 ORDER BY 1$q$);
SELECT * FROM report('no policy yet');
ALTER TABLE rdim ENABLE ROW LEVEL SECURITY;
CREATE POLICY own ON rdim USING (who = current_user);
SELECT n, engaged(sql) AS engine FROM shapes ORDER BY n;
SET ROLE dcache_a;
SELECT * FROM report('user a under the policy');
SELECT who, count(*) AS user_a_rows FROM rdim GROUP BY 1 ORDER BY 1;
SET ROLE dcache_b;
SELECT who, count(*) AS user_b_rows FROM rdim GROUP BY 1 ORDER BY 1;
RESET ROLE;
ALTER TABLE rdim FORCE ROW LEVEL SECURITY;
SELECT who, count(*) AS owner_rows FROM rdim GROUP BY 1 ORDER BY 1;
ALTER TABLE rdim NO FORCE ROW LEVEL SECURITY;
ALTER TABLE rdim DISABLE ROW LEVEL SECURITY;
SET ROLE dcache_b;
SELECT * FROM report('user b, policy off');
RESET ROLE;

DROP OWNED BY dcache_a, dcache_b;
DROP ROLE dcache_a, dcache_b;
DROP FUNCTION report(text, int), engaged(text), answer(text, bool), cache(text), scans(regclass);
DROP TABLE shapes, ev, plain, dim, rdim;
DROP EXTENSION pg_sift;
