-- The scan of an index with a scope (WITH (scope = 'key: fk -> relation(member, scope_key)')) reads one scope key of the join to the scope relation from the scope summary, with the changes since it
-- (a write buffer, deleted facts, pairs that changed), or the join when the summary is not exact for the snapshot (a journal that does not vouch for it); EXPLAIN says which (Scope Read). Every
-- statement is md5 equal through the scope node, through sift with sift.agg_scope off and through the normal plan, in each state.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
-- the scope summary is Format 2.0; the testing library builds new indexes in Format 1.0 when told to
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

-- what a statement does: the same rows through every plan, the Scope Read of the node, and whether workers ran
CREATE FUNCTION scope_run(q text, OUT same boolean, OUT route text, OUT workers boolean)
LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	a text;
	b text;
	c text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO a;
	SET LOCAL sift.agg_scope = off;
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO b;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO c;
	SET LOCAL sift.enable_agg = on;
	SET LOCAL sift.agg_scope = on;
	same := a = b AND b = c;
	route := jsonb_path_query_first(p, '$.**."Scope Read"') #>> '{}';
	workers := coalesce(jsonb_path_query_first(p, '$.**."Workers Launched"')::text::int, 0) > 0;
END $$;

-- 12 tenants; 300 entities, each in one to three tenants, one with none; padded events in a random heap order, some without an entity, some with an entity that has no pair
CREATE TABLE tenant_scope (dim_a_id int, tenant_id int, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT m, 1 + (m * 5 + j * 7) % 12 FROM generate_series(1, 300) m, generate_series(0, m % 3) j WHERE m <> 17 ON CONFLICT DO NOTHING;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN 999 ELSE 1 + (i * 7919) % 320 END,
	timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 30000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v)
	WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
REINDEX INDEX events_col;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (ANALYZE) tenant_scope;
SELECT sift.col_verify('events_col', true);

CREATE TABLE shapes (n int, q text);
INSERT INTO shapes
SELECT n, replace(replace(q, '@', 's.tenant_id = ' || k), 'FROM events e', 'FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id')
FROM (VALUES
	(1, 3, 'SELECT count(*), sum(v), min(id), max(id) FROM events e WHERE @'),
	(2, 3, 'SELECT v, count(*) FROM events e WHERE id > 5000 AND @ GROUP BY v ORDER BY v'),
	(3, 3, 'SELECT count(*), sum(v) FROM events e WHERE @ AND created_at >= timestamptz ''2026-03-01'' AND created_at < timestamptz ''2026-04-01'''),
	(4, 11, 'SELECT count(*), avg(v) FROM events e WHERE v > 4 AND @'),
	(5, 99, 'SELECT count(*), sum(v) FROM events e WHERE @'),
	(6, 5, 'SELECT date_trunc(''month'', created_at) m, count(*) FROM events e WHERE @ GROUP BY 1 ORDER BY 1'),
	(7, 3, 'SELECT id, created_at FROM events e WHERE @ ORDER BY created_at DESC, id DESC LIMIT 20')) s(n, k, q);
CREATE FUNCTION phase() RETURNS TABLE (n int, same boolean, route text) LANGUAGE sql AS $$
	SELECT s.n, r.same, r.route FROM shapes s, LATERAL scope_run(s.q) r ORDER BY s.n
$$;

-- built (key 99 has no pair)
SELECT * FROM phase();

-- workers allowed and the price of a Gather at zero: every read answers as the join
SET max_parallel_workers_per_gather = 4;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT bool_and(r.same) AS all_same FROM shapes s, LATERAL scope_run(s.q) r;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;

-- a journal that does not vouch for the summary (a REINDEX of its index is a new lineage), and the scope index made again
REINDEX INDEX tenant_scope_journal;
SELECT * FROM phase();
REINDEX INDEX events_col;
SELECT * FROM phase();

-- facts deleted: the heap says which entries are live, in every key of their entity; a VACUUM and a rewrite keep it so
DELETE FROM events WHERE id % 13 = 0 AND id <= 30000;
SELECT * FROM phase();
VACUUM events;
SELECT * FROM phase();
SELECT sift.col_verify('events_col', true);

-- rows in a write buffer: the changes since the summary
INSERT INTO events SELECT 30000 + i, 1 + i % 300, timestamptz '2026-02-01', 1, 'y' FROM generate_series(1, 200) i;
SELECT * FROM phase();

-- promoted, and the same after a merge (col_scope_maint has the states of the maintenance)
SELECT sift.col_promote('events_col') >= 0 AS promoted;
SELECT * FROM phase();
SELECT sift.col_merge('events_col', true) >= 0 AS merged;
SELECT * FROM phase();
SELECT sift.col_verify('events_col', true);

-- keys of a few rows each, 150 of them
CREATE TABLE small_scope (dim_a_id int, tenant_id int, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO small_scope SELECT m, 1 + (m * 7) % 150 FROM generate_series(1, 1500) m;
INSERT INTO small_scope SELECT m, 1 + (m * 7 + 11) % 150 FROM generate_series(1, 1500) m WHERE m % 10 = 0 ON CONFLICT DO NOTHING;
CREATE INDEX small_scope_journal ON small_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE small_events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int) WITH (autovacuum_enabled = false);
INSERT INTO small_events SELECT i, 1 + (i * 13) % 1600, timestamptz '2026-01-01' + (i % 200) * interval '1 day', i % 10 FROM generate_series(1, 6000) i;
CREATE INDEX small_col ON small_events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> small_scope(dim_a_id, tenant_id)');
REINDEX INDEX small_col;
VACUUM (FREEZE, ANALYZE) small_events;
SELECT sift.col_verify('small_col', true);
SELECT n, r.same, r.route
FROM (VALUES (1, 1), (2, 77), (3, 150), (4, 151)) k(n, t),
	LATERAL scope_run('SELECT count(*), sum(id), max(created_at) FROM small_events e JOIN small_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = ' || k.t) r
ORDER BY n;

-- the journal of the scope relation names the members of the pairs deleted and added (docs/COL-CONNECT.md "The rule"; col_scope_maint and col_scope_patch have the refill and the other shapes)
DELETE FROM tenant_scope WHERE tenant_id = 3 AND dim_a_id % 5 = 0;
INSERT INTO tenant_scope VALUES (1, 3), (2, 3), (301, 3) ON CONFLICT DO NOTHING;
SELECT * FROM phase();

DROP TABLE small_events, small_scope;
DROP TABLE shapes, events, tenant_scope;
DROP FUNCTION phase(), scope_run(text);
DROP EXTENSION pg_sift;
