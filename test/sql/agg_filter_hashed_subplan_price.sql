-- A NOT IN of a list the planner hashes is a test PostgreSQL evaluates for each distinct value of the column (a call of the subplan and a probe of its hash,
-- 68 ns a value), and each tuple of a value met before finds the value's entry in a table of the values that misses the cache once they are many: 1,000,000
-- rows of 1,000 values run in 16 ms, of 100,000 values in 51 ms and of 632,000 values (in random order) in 199 ms, which the normal plan does in 125 to 176 ms.
-- The scan is priced for that, so it keeps the statement for a column of few values and leaves it to the normal plan for one of many. The answers are those of the normal plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SELECT setseed(0.5);

CREATE TABLE hs_fact (id int, few bigint, many bigint) WITH (autovacuum_enabled = false);
INSERT INTO hs_fact SELECT g, (random() * 1000)::bigint, (random() * 1000000)::bigint FROM generate_series(1, 1000000) g;
CREATE TABLE hs_keys (k bigint);
INSERT INTO hs_keys SELECT g * 3 FROM generate_series(1, 1000) g;
SET client_min_messages = warning;
CREATE INDEX hs_fact_col ON hs_fact USING sift_col (id, few, many);
RESET client_min_messages;
VACUUM (FREEZE, ANALYZE) hs_fact, hs_keys;

CREATE TABLE shapes (slug text, sql text);
INSERT INTO shapes VALUES
 ('not_in_few_values', 'SELECT count(*) FROM hs_fact WHERE few NOT IN (SELECT k FROM hs_keys)'),
 ('not_in_many_values', 'SELECT count(*) FROM hs_fact WHERE many NOT IN (SELECT k FROM hs_keys)'),
 ('not_in_many_values_grouped', 'SELECT few, count(*) FROM hs_fact WHERE many NOT IN (SELECT k FROM hs_keys) GROUP BY 1');

CREATE FUNCTION plan_of(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal plan'; BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN r := 'scan'; END IF;
	END LOOP;
	RETURN r;
END $$;
CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	PERFORM set_config('sift.agg_force', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;

SELECT slug, plan_of(sql) AS plan FROM shapes ORDER BY slug;
SELECT slug, answer(sql, true) = answer(sql, false) AS same_answers FROM shapes ORDER BY slug;

DROP FUNCTION plan_of(text), answer(text, bool);
DROP TABLE shapes, hs_fact, hs_keys;
DROP EXTENSION pg_sift;
