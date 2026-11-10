-- A table of sparse keys (read as rows, not as classes) that only tells whether a key exists (EXISTS, NOT EXISTS, IN) is loaded as its keys alone: no directory of the keys is
-- built, so its load is priced by the rows it reads and not by the cache misses of a directory, from the heap or from a column index alike. At a million keys the scan
-- that loads it is the plan that stays (70-150 ms against 530-620 ms of the normal plan). The answers are those of the normal plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE TABLE ko_heap (k int PRIMARY KEY, a smallint) WITH (autovacuum_enabled = false);
INSERT INTO ko_heap SELECT g * 1000, g % 7 FROM generate_series(1, 1000000) g;
CREATE TABLE ko_cols (k int PRIMARY KEY, a smallint) WITH (autovacuum_enabled = false);
INSERT INTO ko_cols SELECT k, a FROM ko_heap;
CREATE TABLE ko_fact (id int, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ko_fact SELECT g, 1000 * (1 + (g * 7) % 1500000), g % 100 FROM generate_series(1, 1000000) g;
SET client_min_messages = warning;
CREATE INDEX ko_cols_col ON ko_cols USING sift_col (k, a);
CREATE INDEX ko_fact_col ON ko_fact USING sift_col (id, k, v);
RESET client_min_messages;
VACUUM (FREEZE, ANALYZE) ko_heap, ko_cols, ko_fact;

CREATE TABLE shapes (slug text, sql text);
INSERT INTO shapes
SELECT t || '_' || s, format(q, t)
FROM (VALUES ('heap'), ('cols')) tables (t),
     (VALUES ('exists', $q$SELECT sum(f.v) FROM ko_fact f WHERE EXISTS (SELECT 1 FROM ko_%1$s d WHERE d.k = f.k)$q$),
             ('not_exists', $q$SELECT sum(f.v) FROM ko_fact f WHERE NOT EXISTS (SELECT 1 FROM ko_%1$s d WHERE d.k = f.k)$q$),
             ('in', $q$SELECT sum(f.v) FROM ko_fact f WHERE f.k IN (SELECT k FROM ko_%1$s)$q$)) shape (s, q);

CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;

-- the key is unique: PostgreSQL 19 plans an EXISTS or IN as an inner join (no "(semi)"), and the keys of an inner join are loaded alone as well (semi_lut_possible)
CREATE FUNCTION plan_of(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal plan'; BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l ~ 'Lookup Joins: ko_\w+( \((semi|anti)\))?$' THEN r := 'scan with the table loaded as keys'; END IF;
		IF l ~ 'sift_probe' THEN r := 'scan probing the table'; END IF;
	END LOOP;
	RETURN r;
END $$;

SELECT slug, plan_of(sql) AS plan FROM shapes ORDER BY slug;
SELECT slug, answer(sql, true) = answer(sql, false) AS same_answers FROM shapes ORDER BY slug;

DROP FUNCTION answer(text, bool), plan_of(text);
DROP TABLE shapes, ko_fact, ko_cols, ko_heap;
DROP EXTENSION pg_sift;
