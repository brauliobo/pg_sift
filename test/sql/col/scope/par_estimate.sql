-- The reads of one tenant over an index whose parts are ordered by the member and the time (a monthly sum with a bound, a sum by member, a count by score), with parallel workers allowed and every
-- cost at zero: each is md5 equal to the same read in one process and to the normal plan, and says how the scope node read it and how many workers it planned and launched.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET jit = off;
-- the scope summary is Format 2.0; the testing library builds new indexes in Format 1.0 when told to
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET max_parallel_workers_per_gather = 2;

CREATE TABLE tenant_scope (dim_a_id int, tenant_id int, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT m, t FROM generate_series(1, 400) m, generate_series(1, 4) t WHERE (m + t) % 3 = 0 OR t = 1 AND m <= 300 ON CONFLICT DO NOTHING;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at date NOT NULL, kind int NOT NULL, score int NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + (i * 7919) % 400, date '2023-01-01' + (i * 1400 / 240000), i % 6, 1 + i % 10, i % 500 FROM generate_series(1, 240000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, score, amount)
	WITH (key_order = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) max_multiplicity(8)');
VACUUM (FREEZE, ANALYZE) events, tenant_scope;

-- the reads of one tenant
CREATE TABLE reads (name text PRIMARY KEY, q text);
INSERT INTO reads VALUES
 ('sum by month, a bound', $q$SELECT date_trunc('month', created_at) m, count(*), sum(amount) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 1 AND created_at >= '2024-01-01' GROUP BY 1$q$),
 ('sum by member', $q$SELECT e.dim_a_id, count(*), sum(amount) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 1 GROUP BY 1$q$),
 ('count by score', $q$SELECT score, count(*) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 1 GROUP BY 1$q$);
CREATE FUNCTION rows_md5(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO r;
	RETURN r;
END $$;
CREATE FUNCTION plan_of(q text, OUT same boolean, OUT route text, OUT planned int, OUT launched int) LANGUAGE plpgsql AS $$
DECLARE p jsonb; a text; b text; c text;
BEGIN
	PERFORM set_config('max_parallel_workers_per_gather', '2', true);
	PERFORM set_config('parallel_setup_cost', '0', true);
	PERFORM set_config('parallel_tuple_cost', '0', true);
	PERFORM set_config('min_parallel_table_scan_size', '0', true);
	EXECUTE 'EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	planned := coalesce(jsonb_path_query_first(p, '$.**."Workers Planned"')::text::int, 0);
	launched := coalesce(jsonb_path_query_first(p, '$.**."Workers Launched"')::text::int, 0);
	route := jsonb_path_query_first(p, '$.**."Scope Read"') #>> '{}';
	a := rows_md5(q);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	b := rows_md5(q);
	PERFORM set_config('sift.enable_agg', 'off', true);
	c := rows_md5(q);
	PERFORM set_config('sift.enable_agg', 'on', true);
	same := a = b AND b = c;
END $$;
SELECT name, (plan_of(q)).* FROM reads ORDER BY name COLLATE "C";
DROP FUNCTION plan_of(text), rows_md5(text);
DROP TABLE reads, events, tenant_scope CASCADE;
DROP EXTENSION pg_sift;
