-- A scope read prices the units it reads, not the entity units the index also holds: its units are cut by the scope key and the time, so a month bound cuts few of them and a unit holds the rows of
-- many members, where an entity unit holds one member over the whole time. A monthly sum that the units answer from their pre-aggregates costs under 200 units (the entity statistics priced it at the
-- rows of the whole range), a sum by member that reads their rows over 250 and has the workers that were asked for, and a count by score that the pre-aggregates answer says so and keeps one process.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET jit = off;
SET sift.scope_max_multiplicity = 8;
SET max_parallel_workers_per_gather = 2;

CREATE TABLE tenant_scope (dim_a_id int, tenant_id int, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT m, t FROM generate_series(1, 400) m, generate_series(1, 4) t WHERE (m + t) % 3 = 0 OR t = 1 AND m <= 300 ON CONFLICT DO NOTHING;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at date NOT NULL, kind int NOT NULL, score int NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + (i * 7919) % 400, date '2023-01-01' + (i * 1400 / 240000), i % 6, 1 + i % 10, i % 500 FROM generate_series(1, 240000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, score, amount)
	WITH (key_order = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
VACUUM (FREEZE, ANALYZE) events, tenant_scope;

-- the reads of one tenant: what the node plans (workers asked for with the costs at zero, and with the default costs) and what it runs
CREATE TABLE reads (name text PRIMARY KEY, q text);
INSERT INTO reads VALUES
 ('sum by month, a bound', $q$SELECT date_trunc('month', created_at) m, count(*), sum(amount) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) AND created_at >= '2024-01-01' GROUP BY 1$q$),
 ('sum by member', $q$SELECT dim_a_id, count(*), sum(amount) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) GROUP BY 1$q$),
 ('count by score', $q$SELECT score, count(*) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) GROUP BY 1$q$);
CREATE FUNCTION plan_of(q text, OUT planned int, OUT launched int, OUT serial_because text, OUT cost_under_200 boolean, OUT cost_over_250 boolean) LANGUAGE plpgsql AS $$
DECLARE p jsonb; cost float8;
BEGIN
	-- the price of the read in one process, as the node's units are priced (a cost unit is a few microseconds)
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	EXECUTE 'EXPLAIN (COSTS ON, FORMAT JSON) ' || q INTO p;
	cost := jsonb_path_query_first(p, '$.**."Total Cost"')::text::float8;
	cost_under_200 := cost < 200;
	cost_over_250 := cost > 250;
	-- the workers asked for with every cost at zero: the node has them where its units are read, not where its pre-aggregates answer
	PERFORM set_config('max_parallel_workers_per_gather', '2', true);
	PERFORM set_config('parallel_setup_cost', '0', true);
	PERFORM set_config('parallel_tuple_cost', '0', true);
	PERFORM set_config('min_parallel_table_scan_size', '0', true);
	PERFORM set_config('sift.agg_par_min_rows', '0', true);
	EXECUTE 'EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	planned := coalesce(jsonb_path_query_first(p, '$.**."Workers Planned"')::text::int, 0);
	launched := coalesce(jsonb_path_query_first(p, '$.**."Workers Launched"')::text::int, 0);
	serial_because := jsonb_path_query_first(p, '$.**."Serial Because"') #>> '{}';
END $$;
SELECT name, (plan_of(q)).* FROM reads ORDER BY name COLLATE "C";
DROP FUNCTION plan_of(text);
DROP TABLE reads, events, tenant_scope CASCADE;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
