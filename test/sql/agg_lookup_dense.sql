-- A lookup table of one int4 key whose other columns are integer leaves of a sift_col index and take few distinct tuples of values is kept as classes of its keys:
-- a table over the keys, one row for each class. The answers are those of the normal plan (keys the table has no row for, NULL columns, rows the transaction
-- changed, a left join, a distinct count by a column of the table, workers), and a table that is not one of those (a key twice, a negative key, too many tuples,
-- a text column, a read of the key as a column) is loaded row by row as before.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;

CREATE TABLE ev (id int, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, i % 3300, i % 11 FROM generate_series(1, 40000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, k, v);

-- 3200 keys in 24 classes, with NULLs; the events of keys 3200 to 3299 have no row
CREATE TABLE dd (k int PRIMARY KEY, flag bool, lvl int2, grade int, d date, big bigint) WITH (autovacuum_enabled = false);
INSERT INTO dd SELECT i, CASE WHEN i % 7 = 0 THEN NULL ELSE i % 2 = 0 END, (i % 3)::int2, CASE WHEN i % 5 = 0 THEN NULL ELSE i % 4 END,
	date '2026-01-01' + i % 2, i % 2 * 10000000000 FROM generate_series(0, 3199) i;
CREATE INDEX dd_col ON dd USING sift_col (k, flag, lvl, grade, d, big);
-- the same rows with a key twice, a negative key, a tuple of values for each key, and a text column
CREATE TABLE dr (k int, lvl int2, grade int) WITH (autovacuum_enabled = false);
INSERT INTO dr SELECT i, i % 3, i % 4 FROM generate_series(0, 3199) i;
INSERT INTO dr VALUES (5, 1, 1);
CREATE INDEX dr_col ON dr USING sift_col (k, lvl, grade);
CREATE TABLE dn (k int, lvl int2) WITH (autovacuum_enabled = false);
INSERT INTO dn SELECT i - 100, i % 3 FROM generate_series(0, 3299) i;
CREATE INDEX dn_col ON dn USING sift_col (k, lvl);
CREATE TABLE dw (k int PRIMARY KEY, lvl int, grade int) WITH (autovacuum_enabled = false);
INSERT INTO dw SELECT i, i, i % 4 FROM generate_series(0, 5999) i;
CREATE INDEX dw_col ON dw USING sift_col (k, lvl, grade);
CREATE TABLE dt (k int PRIMARY KEY, t text, lvl int) WITH (autovacuum_enabled = false);
INSERT INTO dt SELECT i, 'kind' || i % 5, i % 3 FROM generate_series(0, 3199) i;
CREATE INDEX dt_col ON dt USING sift_col (k, t, lvl);
VACUUM (FREEZE, ANALYZE) ev, dd, dr, dn, dw, dt;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT dd.lvl, dd.grade, count(*), sum(ev.v) FROM ev JOIN dd ON dd.k = ev.k GROUP BY 1, 2 ORDER BY 1, 2$q$),
 (2, $q$SELECT dd.flag, dd.d, dd.big, count(*) FROM ev LEFT JOIN dd ON dd.k = ev.k GROUP BY 1, 2, 3 ORDER BY 1, 2, 3$q$),
 (3, $q$SELECT ev.id % 5, count(DISTINCT ev.k), count(DISTINCT ev.k) FILTER (WHERE dd.lvl = 1), count(DISTINCT ev.k) FILTER (WHERE (dd.grade & 1) <> 0)
		FROM ev JOIN dd ON dd.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (4, $q$SELECT dd.lvl, count(*) FROM ev JOIN dd ON dd.k = ev.k AND dd.flag GROUP BY 1 ORDER BY 1$q$),
 (5, $q$SELECT dd.lvl + dd.grade, count(*), count(dd.flag) FROM ev JOIN dd ON dd.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (6, $q$SELECT dr.lvl, count(*) FROM ev JOIN dr ON dr.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (7, $q$SELECT dn.lvl, count(*) FROM ev JOIN dn ON dn.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (8, $q$SELECT dw.lvl, dw.grade, count(*) FROM ev JOIN dw ON dw.k = ev.k GROUP BY 1, 2 ORDER BY 1, 2$q$),
 (9, $q$SELECT dt.t, dt.lvl, count(*) FROM ev JOIN dt ON dt.k = ev.k GROUP BY 1, 2 ORDER BY 1, 2$q$),
 (10, $q$SELECT dd.lvl, count(DISTINCT dd.k) FROM ev JOIN dd ON dd.k = ev.k GROUP BY 1 ORDER BY 1$q$);

-- the rows of the answer, and the rows the load made classes of
CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;
CREATE FUNCTION classed(sql text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; r int := 0; BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ' || sql LOOP
		IF l ~ '(\d+) rows classed' THEN r := substring(l from '(\d+) rows classed')::int; END IF;
	END LOOP;
	RETURN r;
END $$;
CREATE FUNCTION report(stage text) RETURNS TABLE (stage text, shape int, same_answers bool, classed bool, not_empty bool) LANGUAGE sql AS $$
	SELECT stage, n, answer(sql, true) = answer(sql, false), classed(sql) > 0, answer(sql, true) <> '' FROM shapes ORDER BY n
$$;

SELECT * FROM report('settled');

-- rows the heap changed after the index saw them: updated, deleted, inserted (the write buffer), in the transaction that reads them
BEGIN;
UPDATE dd SET lvl = 2, grade = 3, flag = NULL WHERE k % 31 = 0;
DELETE FROM dd WHERE k % 53 = 0;
INSERT INTO dd SELECT i, true, 1, 1, date '2026-03-01', 7 FROM generate_series(3200, 3249) i;
INSERT INTO ev SELECT i, 3200 + i % 100, 1 FROM generate_series(40001, 40500) i;
SELECT * FROM report('in the transaction');
ROLLBACK;

-- committed, not vacuumed: pages that are not all-visible
UPDATE dd SET grade = 2 WHERE k % 29 = 0;
DELETE FROM dd WHERE k % 61 = 0;
SELECT * FROM report('committed, not vacuumed');
VACUUM dd;
SELECT * FROM report('vacuumed');

-- the workers read the classes from the leader's shared memory
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET sift.agg_par_min_rows = 0;
SELECT * FROM report('workers');

-- switched off with the load from the leaves: the rows are loaded one by one
SET sift.agg_lookup_columns = off;
SELECT * FROM report('lookup columns off');

DROP FUNCTION report(text), classed(text), answer(text, bool);
DROP TABLE shapes, ev, dd, dr, dn, dw, dt;
DROP EXTENSION pg_sift;
