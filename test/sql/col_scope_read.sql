-- The scan of an index with a scope (WITH (scope = 'key: fk -> relation(member, scope_key)')) reads the rows of one scope key from the scope segments of the index: the units of
-- the other keys are not read, and the rows are those of the join to the scope relation. Every state the scope segments cannot answer for (a setting, a write buffer, a journal that
-- does not vouch, a set of keys counted once) is read through the entity rows, and EXPLAIN says why. The test form of the conjunct is the marker sift.in_scope(index, key_name, fk, key).
-- The scope relation has its journal index (a scope index needs it); the first scope index of a relation starts its journal at the end of its build, so the stamps of the segments are taken by
-- the REINDEX that follows it, and the reads say "unfilled" until then.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;

-- what a statement does: the same rows as the plain join, the scope read (or why not), the units of the scope segments it read and skipped, the entries it read
CREATE FUNCTION scope_run(q text, plain text, OUT same boolean, OUT stored boolean, OUT why text, OUT units_read int, OUT units_skipped int, OUT entries int, OUT workers boolean)
LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	a text;
	b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || plain || ') t' INTO b;
	SET LOCAL sift.enable_agg = on;
	same := a = b;
	why := jsonb_path_query_first(p, '$.**."Scope Fallback"') #>> '{}';
	stored := jsonb_path_exists(p, '$.**."Scope Units Read"');
	units_read := coalesce(jsonb_path_query_first(p, '$.**."Scope Units Read"')::text::int, 0);
	units_skipped := coalesce(jsonb_path_query_first(p, '$.**."Scope Units Skipped"')::text::int, 0);
	entries := coalesce(jsonb_path_query_first(p, '$.**."Scope Entries Read"')::text::int, 0);
	workers := jsonb_path_exists(p, '$.**."Workers Launched"');
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

CREATE TABLE shapes (n int, q text, plain text);
INSERT INTO shapes
SELECT n, replace(q, '@', 'sift.in_scope(''events_col''::regclass, ''tenant_id'', e.dim_a_id, ' || k || ')'),
	replace(plain, '@', 'e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ' || k || ')')
FROM (VALUES
	(1, 3, 'SELECT count(*), sum(v), min(id), max(id) FROM events e WHERE @'),
	(2, 3, 'SELECT v, count(*) FROM events e WHERE id > 5000 AND @ GROUP BY v ORDER BY v'),
	(3, 3, 'SELECT count(*), sum(v) FROM events e WHERE @ AND created_at >= timestamptz ''2026-03-01'' AND created_at < timestamptz ''2026-04-01'''),
	(4, 11, 'SELECT count(*), avg(v) FROM events e WHERE v > 4 AND @'),
	(5, 99, 'SELECT count(*), sum(v) FROM events e WHERE @'),
	(6, 5, 'SELECT date_trunc(''month'', created_at) m, count(*) FROM events e WHERE @ GROUP BY 1 ORDER BY 1'),
	(7, 3, 'SELECT id, created_at FROM events e WHERE @ ORDER BY created_at DESC, id DESC LIMIT 20')) s(n, k, q),
	LATERAL (SELECT q AS plain) p;
CREATE FUNCTION phase() RETURNS TABLE (n int, same boolean, stored boolean, why text, fewer_units boolean) LANGUAGE sql AS $$
	SELECT s.n, r.same, r.stored, r.why, r.units_skipped > 0 FROM shapes s, LATERAL scope_run(s.q, s.plain) r ORDER BY s.n
$$;

-- built: every shape reads the scope units of its key and skips those of the others (key 99 has no pair: no unit)
SELECT * FROM phase();

-- a scope read is decided once, by the leader (the verdict, the changed members and the patch are made from the snapshot of the statement): with workers allowed and the price of a
-- Gather at zero it still runs in one process, and answers as the join
SET max_parallel_workers_per_gather = 4;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT bool_and(r.same) AS all_same, bool_and(r.stored) AS all_stored, bool_or(r.workers) AS workers_launched FROM shapes s, LATERAL scope_run(s.q, s.plain) r;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;

-- the setting off: the marker is no conjunct of the scan, the statement is the one as written
SET sift.agg_scope = off;
SELECT * FROM phase();
RESET sift.agg_scope;

-- a journal that does not vouch for the stamp (a REINDEX of its index is a new lineage): the entity rows are read until the scope index is made again
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

-- rows in a write buffer have no scope entries: the entity rows are read, with the members of the key
INSERT INTO events SELECT 30000 + i, 1 + i % 300, timestamptz '2026-02-01', 1, 'y' FROM generate_series(1, 200) i;
SELECT * FROM phase();

-- promoted: the rows of the buffer are an entity segment and scope segments of the same switch of the directory, so the scope units are read again; and the same after a merge (col_scope_maint has the states of the maintenance)
SELECT sift.col_promote('events_col') >= 0 AS promoted;
SELECT * FROM phase();
SELECT sift.col_merge('events_col', true) >= 0 AS merged;
SELECT * FROM phase();
SELECT sift.col_verify('events_col', true);

-- keys of a few rows share units of the scope segment (col/order.c): a key's rows are told from the other keys' by the test of the derived key, and the units of the other keys are still skipped
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
SELECT count(DISTINCT (gen, unit)) < count(DISTINCT key) AS units_shared FROM sift._test_col_scope_entries('small_col');
SELECT n, r.same, r.stored, r.why, r.units_skipped > 0 AS fewer_units
FROM (VALUES (1, 1), (2, 77), (3, 150), (4, 151)) k(n, t),
	LATERAL scope_run(
		'SELECT v, count(*), sum(id), max(created_at) FROM small_events e WHERE sift.in_scope(''small_col''::regclass, ''tenant_id'', e.dim_a_id, ' || k.t || ') GROUP BY v ORDER BY v',
		'SELECT v, count(*), sum(id), max(created_at) FROM small_events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM small_scope WHERE tenant_id = ' || k.t || ') GROUP BY v ORDER BY v') r
ORDER BY n;

-- the journal of the scope relation vouches for the stamp; pairs deleted and added are read through the entity path for their members (docs/COL-CONNECT.md "The rule"), a top-N reads the entity rows (col_scope_maint and col_scope_patch have the refill and the other shapes)
DELETE FROM tenant_scope WHERE tenant_id = 3 AND dim_a_id % 5 = 0;
INSERT INTO tenant_scope VALUES (1, 3), (2, 3), (301, 3) ON CONFLICT DO NOTHING;
SELECT * FROM phase();

DROP TABLE small_events, small_scope;
DROP TABLE shapes, events, tenant_scope;
DROP FUNCTION phase(), scope_run(text, text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
