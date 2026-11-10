-- The latest row of an entity out of thousands (ORDER BY ts DESC LIMIT 1 WHERE ent = n, alone or in a LATERAL per entity) is not read by walking the order array of ts: the heap row
-- of each entry would test ent, and the statistics' pass rate (one in the entities) asks for more fetches than the walk may make, so the planner prices the units. The executor
-- walked all the same until its budget (a quarter of the heap's pages) ran out and then read the units: now the plan tells it not to start (the fetches of the walk, per probe,
-- are what the Top-N Run line reports).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE ev (id int, ent int, ts int, pad text) WITH (autovacuum_enabled = false);
ALTER TABLE ev ALTER pad SET STORAGE PLAIN;
INSERT INTO ev SELECT i, (i::bigint * 7919) % 2003, i, repeat('x', 200) FROM generate_series(1, 60000) i;
CREATE INDEX ev_i ON ev USING sift_col (id, ent, ts) WITH (order_array_columns = 'ts');
CREATE TABLE ents AS SELECT g AS id FROM generate_series(1, 20) g;
VACUUM (FREEZE, ANALYZE) ev, ents;

CREATE FUNCTION walk_fetches(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := -1;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ' || q LOOP
		IF l ~ 'Top-N Run:' THEN n := substring(l FROM '([0-9]+) heap fetches')::int; END IF;
	END LOOP;
	RETURN n;
END $$;

SET sift.agg_force = on;
SELECT walk_fetches('SELECT ts FROM ev WHERE ent = 77 ORDER BY ts DESC LIMIT 1') AS one_entity,
	walk_fetches('SELECT e.id, x.ts FROM ents e LEFT JOIN LATERAL (SELECT ts FROM ev WHERE ent = e.id ORDER BY ts DESC LIMIT 1) x ON true') AS per_entity;
RESET sift.agg_force;

-- the same rows with the normal plan
CREATE FUNCTION walk_rows(push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config('sift.agg_force', push::text, true);
	EXECUTE 'SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (SELECT e.id, x.ts FROM ents e LEFT JOIN LATERAL (SELECT ts FROM ev WHERE ent = e.id ORDER BY ts DESC LIMIT 1) x ON true) x' INTO r;
	RETURN r;
END $$;
SELECT walk_rows(true) = walk_rows(false) AS same_rows;

DROP FUNCTION walk_fetches(text), walk_rows(bool);
DROP TABLE ev, ents;
DROP EXTENSION pg_sift;
