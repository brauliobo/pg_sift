-- A scope read of a member whose pairs changed (the patch) or that sits in a write buffer (the delta) reads its entity rows, which hold only the leaves the scan asks for. A statement that reads a
-- value the scan computes from a leaf (the length of an array, the members of an object by key, the type of a path, the bound of a range) asks for the leaf the value comes from: the rows of the
-- patch and of the delta must carry it, or the value is NULL there and the conjunct drops the row. Table-driven: each such expression, over the units alone, after the pairs of 14 members moved
-- (patch) and after rows were inserted for members of that and another key (delta); every row is that of the plain join.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;

CREATE TABLE tenant_scope (dim_a_id int, tenant_id int, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT m, 1 + m % 4 FROM generate_series(1, 120) m;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, doc jsonb, metrics jsonb, span int4range, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + i % 130,
	jsonb_build_object('kind', CASE i % 3 WHEN 0 THEN to_jsonb('a'::text) WHEN 1 THEN to_jsonb(1) ELSE to_jsonb('c'::text) END, 'parts', coalesce((SELECT jsonb_agg(jsonb_build_object('cat', 'c' || j)) FROM generate_series(1, i % 4) j), '[]')),
	jsonb_build_object('m1', i % 7, 'm2', i % 5), int4range(i % 10, i % 10 + 5), repeat('x', 100)
FROM generate_series(1, 6000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, doc, metrics, span)
	WITH (paths = 'doc.kind text, doc.parts[]{cat text}, metrics.* float8', scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
REINDEX INDEX events_col;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (ANALYZE) tenant_scope;

CREATE FUNCTION scope_run(q text, plain text, OUT same boolean, OUT stored boolean, OUT patched boolean, OUT delta boolean) LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	a text;
	b text;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	EXECUTE 'SELECT string_agg(t::text, ''|'' ORDER BY t::text) FROM (' || q || ') t' INTO a;
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE 'SELECT string_agg(t::text, ''|'' ORDER BY t::text) FROM (' || plain || ') t' INTO b;
	PERFORM set_config('sift.enable_agg', 'on', true);
	same := a IS NOT DISTINCT FROM b;
	stored := jsonb_path_exists(p, '$.**."Scope Units Read"');
	patched := coalesce(jsonb_path_query_first(p, '$.**."Scope Patched Rows"')::text::int, 0) > 0;
	delta := coalesce(jsonb_path_query_first(p, '$.**."Scope Delta Rows"')::text::int, 0) > 0;
END $$;

-- c: the condition on the events e, the scope key is 2
CREATE TABLE shapes (n int, name text, c text);
INSERT INTO shapes VALUES
	(1, 'array length', 'jsonb_array_length(e.doc -> ''parts'') = 2'),
	(2, 'array length, not null', 'jsonb_array_length(e.doc -> ''parts'') IS NOT NULL'),
	(3, 'member of an object', '(e.metrics -> ''m1'')::float8 > 3'),
	(4, 'text of a member', '(e.metrics ->> ''m2'')::float8 < 2'),
	(5, 'type of a path', 'jsonb_typeof(e.doc -> ''kind'') = ''string'''),
	(6, 'bound of a range', 'lower(e.span) >= 5'),
	(7, 'upper bound of a range', 'upper(e.span) < 9');

CREATE FUNCTION run_shapes(state text) RETURNS TABLE (state text, shape text, same boolean, stored boolean, patched boolean, delta boolean) LANGUAGE sql AS $$
	SELECT $1, h.name, r.*
	FROM shapes h, LATERAL scope_run(
		'SELECT count(*), sum(e.id) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 2 AND ' || h.c,
		'SELECT count(*), sum(e.id) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 2 AND ' || h.c) r
	ORDER BY h.n
$$;

-- every row equals the join's, and the scope units were read (not the entity rows)
SELECT * FROM run_shapes('units alone');
UPDATE tenant_scope SET tenant_id = 2 WHERE dim_a_id % 9 = 0 AND tenant_id <> 2;
SELECT * FROM run_shapes('pairs moved');
INSERT INTO events SELECT 10000 + i, 1 + i % 130, jsonb_build_object('kind', 'a', 'parts', jsonb_build_array(jsonb_build_object('cat', 'x'), jsonb_build_object('cat', 'y'))),
	jsonb_build_object('m1', i % 7, 'm2', i % 5), int4range(i % 10, i % 10 + 5), 'y' FROM generate_series(1, 400) i;
SELECT * FROM run_shapes('write buffer');

DROP FUNCTION run_shapes(text);
DROP FUNCTION scope_run(text, text);
DROP TABLE shapes, events, tenant_scope;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
