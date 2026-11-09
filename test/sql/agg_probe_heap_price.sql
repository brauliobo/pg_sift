-- The direct probe of a correlated child (an index scan of the child's btree for each parent) is priced for the heap row each entry fetches: the page visit of a
-- table not in the order of the key, and the condition of the child that the btree does not hold, tested on the fetched row. The probe of a parent's children of
-- ten thousand parents and half a million rows costs more than the table of the children read from the leaves of their sift_col index, which the scan takes.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;

CREATE TABLE hp_parent (id int PRIMARY KEY, grp int) WITH (autovacuum_enabled = false);
INSERT INTO hp_parent SELECT g, g % 40 FROM generate_series(1, 50000) g;
CREATE TABLE hp_child (id bigint PRIMARY KEY, parent_id int, amount numeric(12, 2), at timestamp) WITH (autovacuum_enabled = false);
INSERT INTO hp_child SELECT g, 1 + (g::bigint * 7919) % 45000, (g % 1000)::numeric, timestamp '2024-01-01' + (g % 365) * interval '1 day'
FROM generate_series(1, 1000000) g;
CREATE INDEX hp_child_idx ON hp_child (parent_id, at);
CREATE INDEX hp_parent_col ON hp_parent USING sift_col (id, grp);
CREATE INDEX hp_child_col ON hp_child USING sift_col (parent_id, amount, at);
VACUUM (FREEZE, ANALYZE) hp_parent, hp_child;

CREATE FUNCTION plan_joins(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'none'; BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l ~ 'Lookup Joins:' THEN r := trim(l); END IF;
	END LOOP;
	RETURN r;
END $$;

SELECT plan_joins($q$SELECT p.grp, count(*) FROM hp_parent p
 WHERE EXISTS (SELECT 1 FROM hp_child c WHERE c.parent_id = p.id AND c.amount > 990) GROUP BY p.grp$q$) AS joins;

DROP FUNCTION plan_joins(text);
DROP TABLE hp_parent, hp_child;
DROP EXTENSION pg_sift;
