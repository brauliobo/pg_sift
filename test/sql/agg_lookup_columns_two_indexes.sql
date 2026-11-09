-- A lookup table whose join columns are in two sift_col indexes, none of them holding all, is loaded from the leaves of both (the units of the two have the same rows, so the
-- leaves of the second are read as the first's own), not from the heap: as classes of its keys when it is a dense table, as rows when its keys are sparse. Rows inserted
-- since give the two indexes other units, and the load reads the heap then. The answers are those of the normal plan in every case, for a filter on a column of each index,
-- a text column of the second, a LEFT JOIN, NULLs, and rows changed after the indexes were made.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE ti_parent (id int PRIMARY KEY, a int, b int, tag text, sid int) WITH (autovacuum_enabled = false);
INSERT INTO ti_parent SELECT g, g % 7, CASE WHEN g % 13 = 0 THEN NULL ELSE g % 11 END, (ARRAY['x', 'y', 'z', NULL])[1 + g % 4], g * 1000 FROM generate_series(1, 20000) g;
CREATE TABLE ti_child (id int, parent_id int, sid int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ti_child SELECT g, CASE WHEN g % 50 = 0 THEN NULL ELSE 1 + (g * 7919) % 21000 END, 1000 * (1 + (g * 7919) % 21000), g % 100 FROM generate_series(1, 100000) g;
CREATE INDEX ti_parent_col_a ON ti_parent USING sift_col (id, a, sid);
CREATE INDEX ti_parent_col_b ON ti_parent USING sift_col (id, b, tag);
CREATE INDEX ti_child_col ON ti_child USING sift_col (id, parent_id, sid, v);
VACUUM (FREEZE, ANALYZE) ti_parent, ti_child;

CREATE TABLE shapes (slug text, sql text);
INSERT INTO shapes VALUES
 ('dense_classes_two_columns', 'SELECT p.a, p.b, count(*), sum(c.v) FROM ti_child c JOIN ti_parent p ON p.id = c.parent_id GROUP BY 1, 2'),
 ('dense_classes_text_of_second', 'SELECT p.a, p.tag, count(*), sum(c.v) FROM ti_child c JOIN ti_parent p ON p.id = c.parent_id GROUP BY 1, 2'),
 ('dense_classes_filter_on_each', 'SELECT p.b, count(*) FROM ti_child c JOIN ti_parent p ON p.id = c.parent_id WHERE p.a = 3 AND p.b > 4 GROUP BY 1'),
 ('dense_classes_left_join', 'SELECT p.a, p.b, count(*), count(p.id) FROM ti_child c LEFT JOIN ti_parent p ON p.id = c.parent_id GROUP BY 1, 2'),
 ('sparse_keys_rows', 'SELECT p.a, p.b, p.tag, count(*), sum(c.v) FROM ti_child c JOIN ti_parent p ON p.sid = c.sid GROUP BY 1, 2, 3');

CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	PERFORM set_config('sift.agg_force', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;

-- how the lookup table was loaded: from the leaves of indexes, or from the heap
CREATE FUNCTION loaded(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'heap'; BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	PERFORM set_config('sift.agg_force', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || sql LOOP
		IF l ~ 'Lookup: .*column loads' THEN r := 'index leaves'; END IF;
	END LOOP;
	RETURN r;
END $$;

SELECT slug, loaded(sql) AS load, answer(sql, true) = answer(sql, false) AS same_answers FROM shapes ORDER BY slug;

-- rows added, changed and deleted after the indexes were made: both indexes took the same rows into units of the same rows
INSERT INTO ti_parent SELECT g, g % 7, g % 11, 'w', g * 1000 FROM generate_series(20001, 20500) g;
UPDATE ti_parent SET a = 5, tag = 'u' WHERE id % 97 = 0;
DELETE FROM ti_parent WHERE id % 211 = 0;
SELECT slug, loaded(sql) AS load, answer(sql, true) = answer(sql, false) AS same_answers FROM shapes ORDER BY slug;

-- an index made after those rows has other units than the one made before: the heap is read, the answers hold
DROP INDEX ti_parent_col_b;
CREATE INDEX ti_parent_col_b ON ti_parent USING sift_col (id, b, tag);
SELECT slug, loaded(sql) AS load, answer(sql, true) = answer(sql, false) AS same_answers FROM shapes ORDER BY slug;

DROP FUNCTION answer(text, bool), loaded(text);
DROP TABLE shapes, ti_child, ti_parent;
DROP EXTENSION pg_sift;
