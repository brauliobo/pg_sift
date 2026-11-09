-- A LEFT JOIN of two tables that both have a sift_col index: the scan may run over the child with the preserved parent as its lookup, as well as over the parent with the child as
-- its lookup, and the price chooses. The answers are those of the normal plan, serial and with workers, before and after writes to the parent and to the child (a parent
-- with no child, a child of no parent, a key changed, rows deleted).
CREATE EXTENSION pg_sift;
SET jit = off;
SET sift.agg_force = on;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;
SET sift.agg_dimmap_kb = 0;

CREATE TABLE parent (id int PRIMARY KEY, region int, kind int, name text) WITH (autovacuum_enabled = false);
INSERT INTO parent SELECT i, i % 100, i % 10, CASE WHEN i % 17 = 0 THEN NULL ELSE 'p' || i END FROM generate_series(1, 20000) i;
CREATE TABLE child (id int PRIMARY KEY, parent_id int, v int, ts date, flag bool) WITH (autovacuum_enabled = false);
-- 200,000 children of 18,000 parents (the others have none), 1,000 of keys no parent has, and some NULL keys
INSERT INTO child SELECT i, CASE WHEN i % 200 = 0 THEN NULL WHEN i % 200 = 1 THEN 30000 + i % 1000 ELSE 1 + (i * 7) % 18000 END, (i * 13) % 100,
	date '2026-01-01' + (i % 300), i % 3 = 0 FROM generate_series(1, 200000) i;
CREATE INDEX parent_col ON parent USING sift_col (id, region, kind, name);
CREATE INDEX child_col ON child USING sift_col (id, parent_id, v, ts, flag);
VACUUM (FREEZE, ANALYZE) parent, child;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT p.id, count(c.id) FROM parent p LEFT JOIN child c ON c.parent_id = p.id GROUP BY p.id ORDER BY 2 DESC, 1 LIMIT 10$q$),
 (2, $q$SELECT p.region, count(c.id), count(*) FROM parent p LEFT JOIN child c ON c.parent_id = p.id GROUP BY 1$q$),
 (3, $q$SELECT p.region, sum(c.v) FROM parent p LEFT JOIN child c ON c.parent_id = p.id AND c.flag AND c.ts >= date '2026-06-01' GROUP BY 1$q$),
 (4, $q$SELECT p.region, count(*), count(c.flag), sum(c.v) FROM parent p LEFT JOIN child c ON c.parent_id = p.id AND c.v < 5 WHERE p.id > 100 GROUP BY p.region$q$),
 (5, $q$SELECT p.kind, count(DISTINCT c.v), max(c.ts), min(p.name) FROM parent p LEFT JOIN child c ON c.parent_id = p.id WHERE p.region < 50 GROUP BY 1$q$),
 (6, $q$SELECT count(*) FILTER (WHERE c.id IS NULL), count(c.id), sum(c.v) FROM parent p LEFT JOIN child c ON c.parent_id = p.id$q$),
 (7, $q$SELECT p.name IS NULL, c.flag, count(*) FROM parent p LEFT JOIN child c ON c.parent_id = p.id GROUP BY 1, 2$q$),
 (8, $q$SELECT p.region, count(*) FROM parent p LEFT JOIN child c ON c.parent_id = p.id WHERE c.v > 90 OR c.v IS NULL GROUP BY 1$q$);

CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;
-- the table the scan runs over ('normal' when the statement has no scan)
CREATE FUNCTION scanned(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal'; BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l ~ 'Index: child_col' THEN r := 'child'; ELSIF l ~ 'Index: parent_col' THEN r := 'parent'; END IF;
	END LOOP;
	RETURN r;
END $$;
CREATE FUNCTION report(label text) RETURNS TABLE (stage text, workers int, shape int, same_answers bool, scan text) LANGUAGE plpgsql AS $$
DECLARE w int; BEGIN
	FOREACH w IN ARRAY ARRAY[0, 2] LOOP
		PERFORM set_config('max_parallel_workers_per_gather', w::text, true);
		PERFORM set_config('parallel_setup_cost', '0', true);
		PERFORM set_config('parallel_tuple_cost', '0', true);
		PERFORM set_config('min_parallel_table_scan_size', '0', true);
		PERFORM set_config('sift.agg_par_min_rows', '0', true);
		RETURN QUERY SELECT label, w, n, answer(sql, true) = answer(sql, false), scanned(sql) FROM shapes ORDER BY n;
	END LOOP;
END $$;

SELECT * FROM report('built');

-- the parent written: parents with no child, a parent deleted, a region changed
INSERT INTO parent SELECT i, i % 100, i % 10, 'new' FROM generate_series(20001, 20500) i;
DELETE FROM parent WHERE id % 97 = 0;
UPDATE parent SET region = (region + 1) % 100 WHERE id % 13 = 0;
SELECT * FROM report('parent written');

-- the child written: a child of a parent that had none, a child moved to another parent, a key made NULL, rows deleted
INSERT INTO child SELECT 300000 + i, 20001 + i % 500, i % 50, date '2026-12-01', true FROM generate_series(1, 2000) i;
UPDATE child SET parent_id = 5 WHERE id % 61 = 0;
UPDATE child SET parent_id = NULL WHERE id % 71 = 0;
DELETE FROM child WHERE id % 53 = 2;
SELECT * FROM report('child written');

-- the transaction's own rows
BEGIN;
INSERT INTO parent VALUES (99999, 7, 7, 'own');
INSERT INTO child VALUES (999999, 99999, 1, date '2026-01-01', false), (999998, 99999, 2, date '2026-01-02', true);
UPDATE parent SET region = 8 WHERE id = 5;
SELECT * FROM report('own rows');
ROLLBACK;

VACUUM parent, child;
SELECT * FROM report('vacuumed');

DROP FUNCTION report(text), scanned(text), answer(text, bool);
DROP TABLE shapes, child, parent;
DROP EXTENSION pg_sift;
