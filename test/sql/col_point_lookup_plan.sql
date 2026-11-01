-- A statement that aggregates the rows of a few keys of a column the index looks up by itself (an equality or an IN list of up to 512 constants, a point directory
-- of heap addresses or an order array on the column) keeps the index and bitmap scans of the access method beside SiftAgg, and the costs choose: SiftAgg reads a unit
-- chunk of each key, the index scan a heap page. A group by, a range, a column the index does not look up, or a longer list is SiftAgg's, as before. The answers are the
-- heap's over NULLs, duplicates, DML and the write buffer. The plan is asked with SiftAgg off: it is then the paths the index offers that show (an index or bitmap scan, or
-- the sequential scan that their absence leaves), whatever the size of the table; which of SiftAgg and the index scan wins is a matter of the cost, the gate of the planner and the bench cases.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE pl (id int, dup int, day int, v int, g int, pad text DEFAULT repeat('x', 100));
INSERT INTO pl (id, dup, day, v, g)
SELECT CASE WHEN i % 1009 = 0 THEN NULL ELSE i END, i % 19000, i % 30, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 997 END, i % 7 FROM generate_series(1, 20000) i;
VACUUM (FREEZE, ANALYZE) pl;
CREATE INDEX pl_col ON pl USING sift_col (id, dup, day, v, g) WITH (order_columns = 'id, dup, day', point_tid_columns = 'id', order_array_columns = 'id, dup, day');
CREATE TABLE pl_keys AS SELECT n, string_agg(((i::bigint * 7919) % 20000 + 1)::text, ',' ORDER BY i) AS list
FROM unnest(ARRAY[1, 10, 60, 500, 513]) n, generate_series(1, 513) i WHERE i <= n GROUP BY n;

CREATE FUNCTION pl_path(sql text) RETURNS text LANGUAGE plpgsql SET sift.enable_agg = off AS $$
DECLARE l text; r text := 'seq';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l ~ 'Index Scan|Bitmap Index Scan' THEN r := 'index'; EXIT; END IF;
	END LOOP;
	RETURN r;
END $$;

-- the plan of each shape
SELECT w AS "where", k AS keys, pl_path(format('SELECT count(*), sum(v) FROM pl WHERE %s', replace(w, '$L', (SELECT list FROM pl_keys WHERE n = k)))) AS path
FROM (VALUES ('id = 777', 1), ('id IN ($L)', 10), ('id IN ($L)', 60), ('dup = 777', 1), ('dup IN ($L)', 10), ('day = 7', 1), ('day IN ($L)', 10)) t(w, k);
SELECT q AS statement, pl_path(q) AS path FROM unnest(ARRAY[
	'SELECT g, count(*) FROM pl WHERE id IN (1, 2, 3) GROUP BY g',
	'SELECT count(*) FROM pl WHERE id BETWEEN 100 AND 5000',
	'SELECT count(*) FROM pl WHERE v = 5',
	'SELECT count(*) FROM pl WHERE id = 777 AND v = 5',
	'SELECT count(DISTINCT v) FROM pl WHERE id IN (1, 2, 3)',
	'SELECT id, v FROM pl WHERE id IN (1, 2, 3)'
]) q;

-- the answers: every shape, the access method with the planner's choice against the heap, over the table as loaded, after DML and with a write buffer
CREATE FUNCTION pl_run(w text, agg boolean) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_col_scan', agg::text, true);
	PERFORM set_config('sift.enable_agg', agg::text, true);
	EXECUTE format('SELECT concat_ws(%L, count(*), count(id), sum(v), count(v), sum(dup), min(id), max(id)) FROM pl WHERE %s', ' ', w) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION pl_rows(w text, agg boolean) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_col_scan', agg::text, true);
	PERFORM set_config('sift.enable_agg', agg::text, true);
	EXECUTE format('SELECT coalesce(string_agg(concat_ws(%L, id, dup, v), %L ORDER BY id, dup, v), %L) FROM pl WHERE %s', ',', ';', '-', w) INTO r;
	RETURN r;
END $$;
CREATE TABLE pl_where AS
SELECT w FROM (
	SELECT replace(w, '$L', (SELECT list FROM pl_keys WHERE n = k)) AS w
	FROM (VALUES ('id = 777', 1), ('id = 1009', 1), ('id = 0', 1), ('id IN ($L)', 1), ('id IN ($L)', 10), ('id IN ($L)', 60), ('id IN ($L)', 500), ('id IN ($L)', 513),
		('dup = 777', 1), ('dup IN ($L)', 10), ('dup IN ($L)', 60), ('id = 777 AND v IS NOT NULL', 1), ('id IN ($L) AND v > 500', 60), ('id = ANY (ARRAY[5, 5, 6, NULL])', 1),
		('id IN (1009, 2018, 3027)', 1), ('dup = 0', 1), ('day = 7', 1), ('day IN (1, 2, 3)', 1)) t(w, k)) s;

CREATE FUNCTION pl_check() RETURNS TABLE (checked bigint, differ bigint) LANGUAGE sql AS $$
	SELECT count(*), count(*) FILTER (WHERE pl_run(w, true) IS DISTINCT FROM pl_run(w, false) OR pl_rows(w, true) IS DISTINCT FROM pl_rows(w, false)) FROM pl_where $$;
SELECT * FROM pl_check();

DELETE FROM pl WHERE id % 13 = 0;
UPDATE pl SET id = id + 1000000 WHERE id % 17 = 0;
UPDATE pl SET v = v + 1 WHERE id % 19 = 0;
SELECT * FROM pl_check();
INSERT INTO pl (id, dup, day, v, g) SELECT i, i % 19000, i % 30, i % 5, 1 FROM generate_series(20001, 20400) i;
INSERT INTO pl (id, dup, v, g) VALUES (777, 777, 1, 1), (NULL, 777, NULL, 2);
SELECT * FROM pl_check();
VACUUM pl;
SELECT * FROM pl_check();

DROP TABLE pl, pl_keys, pl_where;
DROP FUNCTION pl_path(text), pl_run(text, boolean), pl_rows(text, boolean), pl_check();
DROP EXTENSION pg_sift;
