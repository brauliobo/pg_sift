-- A correlated min() or max() of a table (SELECT max(ts) FROM ev WHERE ent = e.id, in a select list or a condition) is the first row of the entity in the order of ts, as PostgreSQL
-- plans one at the top of a query with a btree: it was a pass over the entity's rows, or a table grouped by entity and joined (agg/flat/decor.c). The third plan of the statement (the
-- sub-select as ORDER BY ts DESC LIMIT 1 with ts IS NOT NULL) reads one entry of the order array of the entity, and the planner keeps it where it costs less (sift.agg_force
-- takes the scan whatever the price, as the tests of the plan do). Where the entity's rows cannot be told from the array (a loose condition that passes one row in a thousand) the
-- walk is refused at planning and no heap row is fetched. The answers are those of a copy of the table without an index (the oracle) over NULLs in the column, entities without rows, NULL
-- outer keys, both aggregates, a text column under its collation, other conditions of the sub-select, writes, and 100 random statements (fixed seed).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;

CREATE TABLE ev AS SELECT i AS id,
	CASE WHEN i % 61 = 0 THEN NULL ELSE ((i::bigint * 7919) % 60)::int END AS ent,
	CASE WHEN i % 43 = 0 THEN NULL ELSE ((i::bigint * 104729) % 100003)::int END AS ts,
	CASE WHEN i % 29 = 0 THEN NULL ELSE (i * 7) % 30 END AS m,
	CASE WHEN i % 37 = 0 THEN NULL ELSE 'c' || lpad(((i * 17) % 500)::text, 3, '0') END AS code, repeat('x', 800) AS pad
FROM generate_series(1, 3000) i;
CREATE INDEX ev_i ON ev USING sift_col (id, ent, ts, m, code) WITH (order_columns = 'ts', order_array_columns = 'ts', order_array_group = 'ent');
-- (pad: the heap pages that the budget of the walk is a quarter of)
-- entities 0..59 have rows; 60..71 have none; one has a NULL key
CREATE TABLE ents AS SELECT g AS id, g % 7 AS dim FROM generate_series(0, 71) g UNION ALL SELECT NULL, 99;
CREATE TABLE evp AS SELECT * FROM ev;
ANALYZE ev, evp, ents;
VACUUM ev;

-- is the sub-select a top-1 probe of the order array, and how many heap rows does it fetch
CREATE FUNCTION plan_of(q text, OUT global_order boolean, OUT fetches int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	global_order := false; fetches := -1;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'Global Order: true' THEN global_order := true; END IF;
		IF l ~ 'Top-N Run:' THEN fetches := substring(l FROM '([0-9]+) heap fetches')::int; END IF;
	END LOOP;
END $$;
-- the rows, in order, as the oracle has them
CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT md5(coalesce(string_agg(x::text, '','' ORDER BY ord), '''')) FROM (SELECT row_number() OVER () AS ord, * FROM (%s) r) x', replace(q, '@', 'ev')) INTO a;
	EXECUTE format('SELECT md5(coalesce(string_agg(x::text, '','' ORDER BY ord), '''')) FROM (SELECT row_number() OVER () AS ord, * FROM (%s) r) x', replace(q, '@', 'evp')) INTO b;
	IF a IS DISTINCT FROM b THEN RAISE NOTICE '% differs', q; END IF;
	RETURN a IS NOT DISTINCT FROM b;
END $$;

CREATE TABLE tq (n int, q text);
INSERT INTO tq SELECT row_number() OVER (), q FROM unnest(ARRAY[
 'SELECT e.id, (SELECT max(ts) FROM @ WHERE ent = e.id) FROM ents e WHERE e.dim = 3 ORDER BY e.id',
 'SELECT e.id, (SELECT min(ts) FROM @ WHERE ent = e.id) FROM ents e WHERE e.dim = 3 ORDER BY e.id',
 'SELECT e.id, (SELECT max(ts) FROM @ WHERE ent = e.id), (SELECT min(ts) FROM @ WHERE ent = e.id) FROM ents e ORDER BY e.id',
 'SELECT e.id, (SELECT max(ts) FROM @ WHERE ent = e.id AND m > 10) FROM ents e WHERE e.dim IN (1, 2) ORDER BY e.id',
 'SELECT e.id, (SELECT min(ts) FROM @ WHERE ent = e.id AND m IS NULL) FROM ents e WHERE e.dim = 5 ORDER BY e.id',
 'SELECT e.id, (SELECT max(code) FROM @ WHERE ent = e.id) FROM ents e WHERE e.dim = 4 ORDER BY e.id',
 'SELECT e.id, (SELECT min(code) FROM @ WHERE ent = e.id AND ts > 50000) FROM ents e WHERE e.dim = 4 ORDER BY e.id',
 'SELECT e.id, (SELECT max(code COLLATE "C") FROM @ WHERE ent = e.id) FROM ents e WHERE e.dim = 6 ORDER BY e.id',
 'SELECT e.id, (SELECT max(m) FROM @ WHERE ent = e.id) FROM ents e WHERE e.dim = 2 ORDER BY e.id',
 'SELECT e.id, (SELECT max(ts + 1) FROM @ WHERE ent = e.id) FROM ents e WHERE e.dim = 2 ORDER BY e.id',
 'SELECT e.id FROM ents e WHERE (SELECT max(ts) FROM @ WHERE ent = e.id) > 99000 ORDER BY e.id',
 'SELECT e.id FROM ents e WHERE (SELECT min(ts) FROM @ WHERE ent = e.id) IS NULL AND e.dim = 1 ORDER BY e.id',
 'SELECT x.id, x.ts FROM @ x WHERE x.ts = (SELECT max(y.ts) FROM @ y WHERE y.ent = x.ent) ORDER BY x.id',
 'SELECT e.id, (SELECT max(ts) FROM @ WHERE ent = e.id AND (SELECT max(1)) = 1) FROM ents e WHERE e.dim = 0 ORDER BY e.id',
 'SELECT e.id, (SELECT max(ts) FROM @ WHERE ent = e.id OR ent = e.id + 1) FROM ents e WHERE e.dim = 0 ORDER BY e.id',
 'SELECT e.id, (SELECT max(ts) FROM @ WHERE ent = e.id + 1) FROM ents e WHERE e.dim = 0 ORDER BY e.id',
 'SELECT e.id, (SELECT max(ts) FROM @ WHERE ent < e.id AND ent > e.id - 3) FROM ents e WHERE e.dim = 0 ORDER BY e.id',
 'SELECT e.id, (SELECT max(ts) FILTER (WHERE m > 3) FROM @ WHERE ent = e.id) FROM ents e WHERE e.dim = 0 ORDER BY e.id',
 'SELECT e.id, (SELECT max(DISTINCT ts) FROM @ WHERE ent = e.id) FROM ents e WHERE e.dim = 0 ORDER BY e.id',
 'SELECT e.id, (SELECT count(*) FROM @ WHERE ent = e.id) FROM ents e WHERE e.dim = 0 ORDER BY e.id',
 'SELECT g.k, (SELECT max(ts) FROM @ WHERE ent = g.k) FROM (SELECT * FROM generate_series(-2, 75, 2) k) g ORDER BY g.k']) q;

-- the probes: with the scan taken whatever the price, a sub-select of one aggregate and one table is a top-1 of the order array (not a pass over the entity, nor a join) where the array
-- is of its column and the group is its equality; the others keep the aggregate. Three statements are not asked: the prices of their plans are all zero and the choice between them is noise.
SET sift.agg_force = on;
SELECT n, (plan_of(replace(q, '@', 'ev'))).global_order AS global_order, same(q) AS answers FROM tq WHERE n NOT IN (3, 11, 12) ORDER BY n;
SELECT n, same(q) AS answers FROM tq WHERE n IN (3, 11, 12) ORDER BY n;
RESET sift.agg_force;
SELECT n, same(q) AS answers FROM tq ORDER BY n;

-- the walk refused: the group is not the order array's (it is of ts alone), the condition on ent passes few rows and the units answer, with no heap row fetched by the walk
CREATE TABLE ev2 AS SELECT id, ent, ts FROM ev;
CREATE INDEX ev2_i ON ev2 USING sift_col (id, ent, ts) WITH (order_columns = 'ts', order_array_columns = 'ts');
ANALYZE ev2;
SET sift.agg_force = on;
SELECT (plan_of('SELECT e.id, (SELECT max(ts) FROM ev2 WHERE ent = e.id) FROM ents e WHERE e.dim = 3')).fetches AS refused_fetches;
SELECT (plan_of('SELECT e.id, (SELECT ts FROM ev2 WHERE ent = e.id AND ts IS NOT NULL ORDER BY ts DESC LIMIT 1) FROM ents e WHERE e.dim = 3')).fetches AS written_fetches;
RESET sift.agg_force;
DROP TABLE ev2;

-- a condition of another column on the heap row: at one row in 30 the order array is still walked, at one in 500 (a handful of rows in all) the plan keeps the units (no global order); the Top-N Run line reports no heap fetch of the walk in either
SET sift.agg_force = on;
SELECT p.global_order, p.fetches > 0 AS fetched FROM plan_of('SELECT e.id, (SELECT max(ts) FROM ev WHERE ent = e.id AND m = 7) FROM ents e WHERE e.dim = 3') p;
SELECT p.global_order, p.fetches > 0 AS fetched FROM plan_of('SELECT e.id, (SELECT max(ts) FROM ev WHERE ent = e.id AND code = ''c017'') FROM ents e WHERE e.dim = 3') p;
RESET sift.agg_force;

-- writes: the write buffer, an update of the key and of the column, a delete, a rolled back insert; both tables get the same
CREATE FUNCTION twice(sql text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE replace(sql, '@', 'ev');
	EXECUTE replace(sql, '@', 'evp');
END $$;
SELECT twice('INSERT INTO @ (id, ent, ts, m, code) SELECT 3000 + i, (i * 3) % 72, 100003 + i, i % 30, ''c999'' FROM generate_series(1, 100) i');
SELECT twice('UPDATE @ SET ts = ts + 200000 WHERE id % 211 = 0');
SELECT twice('UPDATE @ SET ent = (ent + 1) % 60 WHERE id % 307 = 0');
SELECT twice('DELETE FROM @ WHERE id % 97 = 0');
BEGIN; DELETE FROM ev WHERE m = 3; ROLLBACK;
SET sift.agg_force = on;
SELECT n, same(q) AS answers FROM tq ORDER BY n;
VACUUM ev;
SELECT n, same(q) AS answers FROM tq ORDER BY n;
RESET sift.agg_force;

-- the plan of a stored statement, custom for the first executions and generic after, with the value of a condition of the sub-select as a parameter
CREATE FUNCTION stored_differs() RETURNS int LANGUAGE plpgsql AS $$
DECLARE a text; b text; v int; n int := 0;
BEGIN
	FOR i IN 1..9 LOOP
		v := (ARRAY[5, 25, 0])[1 + i % 3];
		SELECT md5(string_agg(x::text, ',' ORDER BY x.id)) INTO a FROM (SELECT e.id, (SELECT max(ts) FROM ev WHERE ent = e.id AND m > v) FROM ents e WHERE e.dim = 3) x;
		SELECT md5(string_agg(x::text, ',' ORDER BY x.id)) INTO b FROM (SELECT e.id, (SELECT max(ts) FROM evp WHERE ent = e.id AND m > v) FROM ents e WHERE e.dim = 3) x;
		IF a IS DISTINCT FROM b THEN n := n + 1; END IF;
	END LOOP;
	RETURN n;
END $$;
SET sift.agg_force = on;
SELECT stored_differs() AS differ;
RESET sift.agg_force;

-- random statements: the aggregate and its column, the conditions, the outer rows (a range, NULL, absent entities), a select list, a condition, a self comparison
CREATE FUNCTION fuzz_minmax(n int) RETURNS TABLE (statements int, differ int, used_global_order int) LANGUAGE plpgsql AS $$
DECLARE
	aggs text[] := ARRAY['max(ts)', 'min(ts)', 'max(code)', 'min(code)', 'max(m)', 'min(m)', 'max(ts + m)', 'max(id)'];
	conds text[] := ARRAY['', ' AND m > 10', ' AND m IS NULL', ' AND m IS NOT NULL', ' AND ts > 50000', ' AND ts < 1000', ' AND code >= ''c250''', ' AND code = ''c017''',
						  ' AND id % 3 = 0', ' AND ts IS NULL', ' AND m = 7 AND code < ''c300'''];
	q text; shape int; a text; c text; r text; cmp text;
BEGIN
	PERFORM setseed(0.52);
	statements := 0; differ := 0; used_global_order := 0;
	FOR i IN 1..n LOOP
		shape := floor(random() * 4)::int;
		a := aggs[1 + floor(random() * 8)::int];
		c := conds[1 + floor(random() * 11)::int];
		r := (ARRAY['e.dim = 3', 'e.dim IN (1, 5)', 'e.dim = 99', 'e.id < 25', 'e.id > 55', 'true'])[1 + floor(random() * 6)::int];
		cmp := (ARRAY['> 50000', '< 20000', 'IS NULL', 'IS NOT NULL', '= 100002'])[1 + floor(random() * 5)::int];
		q := CASE shape
			WHEN 0 THEN format('SELECT e.id, (SELECT %s FROM @ WHERE ent = e.id%s) FROM ents e WHERE %s ORDER BY e.id', a, c, r)
			WHEN 1 THEN format('SELECT e.id, (SELECT %s FROM @ WHERE ent = e.id%s), (SELECT %s FROM @ WHERE ent = e.id) FROM ents e WHERE %s ORDER BY e.id', a, c, aggs[1 + floor(random() * 8)::int], r)
			WHEN 2 THEN format('SELECT e.id FROM ents e WHERE %s AND (SELECT max(ts) FROM @ WHERE ent = e.id%s) %s ORDER BY e.id', r, c, cmp)
			ELSE format('SELECT x.id FROM @ x WHERE x.id %% 11 = 0 AND x.ts IS NOT DISTINCT FROM (SELECT %s FROM @ y WHERE y.ent = x.ent%s) ORDER BY x.id',
						(ARRAY['max(y.ts)', 'min(y.ts)'])[1 + floor(random() * 2)::int], replace(c, ' AND ', ' AND y.')) 
		END;
		statements := statements + 1;
		IF NOT same(q) THEN differ := differ + 1; END IF;
		IF (plan_of(replace(q, '@', 'ev'))).global_order THEN used_global_order := used_global_order + 1; END IF;
	END LOOP;
	RETURN NEXT;
END $$;
SET sift.agg_force = on;
SELECT statements, differ, used_global_order > 0 AS used_global_order FROM fuzz_minmax(100);
RESET sift.agg_force;

DROP FUNCTION fuzz_minmax(int), stored_differs(), twice(text), same(text), plan_of(text);
DROP TABLE tq, ev, evp, ents;
DROP EXTENSION pg_sift;
