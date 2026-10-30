-- The IN sub-select that a shared CTE's inline copy hoists into a CTE of the WITH (agg_cte_hoist) is a list of keys that is computed whatever the join order. A reader
-- that PostgreSQL keeps (an arm the column index cannot serve: it reads a column the index lacks) joins the list before the table it filters: the table is
-- probed for the keys of the list, not for every key of the scope the estimates (one row for both) leave it to join first. The rows are the normal plan's.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;
CREATE TABLE parents (id int PRIMARY KEY, grp int NOT NULL);
INSERT INTO parents SELECT g, 1 + g % 3 FROM generate_series(1, 600) g;
CREATE TABLE events (id int PRIMARY KEY, parent_id int NOT NULL, ts date NOT NULL, score int NOT NULL, note text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT g, 1 + ((g::bigint * 7919) % 600)::int, date '2025-01-01' + ((g::bigint * 7919) % 360)::int, g % 10, 'n' || g FROM generate_series(1, 60000) g;
CREATE INDEX events_parent ON events (parent_id);
CREATE INDEX events_col ON events USING sift_col (id, parent_id, ts, score);
VACUUM (FREEZE, ANALYZE) events;
ANALYZE parents;

-- the scope (read twice, so a CTE of its own) and the list (ten keys of 600, from a HAVING the estimates take for one row) are both estimated at one row
CREATE FUNCTION keys_query() RETURNS text LANGUAGE sql AS $$
	SELECT $q$WITH scope AS (SELECT id FROM parents WHERE grp > 0 AND (id + 0) % 3 = 1 AND (id + 0) % 5 > -1 AND (id + 0) % 7 >= 0 AND (id + 0) % 11 >= 0 AND id <> -1 AND id % 1 = 0),
	  scoped AS (SELECT e.* FROM scope s JOIN parents p ON p.id = s.id JOIN events e ON e.parent_id = p.id
	             WHERE p.id IN (SELECT parent_id FROM (SELECT parent_id, row_number() OVER (PARTITION BY parent_id % 2 ORDER BY count(*) DESC, parent_id) rn
	                                                   FROM events GROUP BY parent_id HAVING count(*) % 7 = 2 AND sum(score) % 1 = 0 AND min(id) % 1 = 0) r WHERE rn <= 5)),
	  by_month AS (SELECT date_trunc('month', ts) m, sum(score) AS total FROM scoped GROUP BY 1),
	  by_note AS (SELECT left(note, 2) n, count(*) AS c FROM scoped GROUP BY 1)
	SELECT 'month' k, m::text, total FROM by_month UNION ALL SELECT 'note', n, c FROM by_note UNION ALL SELECT 'scope', NULL, count(*) FROM scope$q$
$$;

-- the scan nodes of the plan, and the most times a node reads events by its parent_id index (loops of the probes)
CREATE FUNCTION probes(q text, OUT scan_nodes int, OUT max_probes int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	scan_nodes := 0; max_probes := 0;
	SET sift.agg_force = on;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN scan_nodes := scan_nodes + 1; END IF;
		IF l ~ 'events_parent' THEN max_probes := greatest(max_probes, substring(l from 'loops=(\d+)')::int); END IF;
	END LOOP;
	RESET sift.agg_force;
END $$;
CREATE FUNCTION differences(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE d bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SET sift.agg_force = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	RESET sift.agg_force;
	IF (SELECT count(*) FROM r_off) < 10 THEN RAISE EXCEPTION 'the series has few rows'; END IF;
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) x;
	DROP TABLE r_on, r_off;
	RETURN d;
END $$;

SELECT scan_nodes >= 1 AS aggregate_scan, max_probes <= 10 AS probes_the_keys_of_the_list FROM probes(keys_query());
SELECT differences(keys_query()) AS differences;
DROP TABLE events, parents;
DROP FUNCTION differences(text), probes(text), keys_query();
DROP EXTENSION pg_sift;
