-- The key groups of a scope segment (column format minor 51, docs/COL-SCOPE.md "Key groups"): the units that hold one scope key and no other are a group of their own, with
-- the merged cubes of the units and the tables of the ids the units lack (a unit holds a few rows of each id of its key). An aggregate by id, a top of ids, a
-- count by kind or a range of days over a key is answered from the groups, md5 equal to the join with the sift plan off; the block is checked against the units, every byte of it
-- refused when damaged, and it follows writes, deletes, VACUUM, promotion and merge. Counts and hashes only.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.test_col_unit_rows = 100;

-- key 1 holds ids 1 to 60 (several hundred rows of each), key 2 ids 61 to 66, key 3 id 67 only
CREATE TABLE tenant_scope (dim_a_id int, tenant_id int, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT d, CASE WHEN d <= 60 THEN 1 WHEN d <= 66 THEN 2 ELSE 3 END FROM generate_series(1, 67) d;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');

CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at date, score int, kind text, amount int) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + (i * 7919) % 67, date '2026-01-01' + (i / 40), 1 + i % 10, 'k' || (i % 4), i % 500 FROM generate_series(1, 20000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, score, kind, amount)
	WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
REINDEX INDEX events_col;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (ANALYZE) tenant_scope;

-- the groups are written: the scope segments are of minor 51, and the block checks against the units
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('events_col');
SELECT sift.col_verify('events_col', true);

CREATE TABLE cases (n int, label text, stmt text);
INSERT INTO cases VALUES
	(1, 'count by id, big key', 'SELECT dim_a_id, count(*) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) GROUP BY 1'),
	(2, 'sum by id, big key', 'SELECT dim_a_id, count(*), sum(amount) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) GROUP BY 1'),
	(3, 'top ids, big key', 'SELECT dim_a_id, count(*) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) GROUP BY 1 ORDER BY 2 DESC, 1 LIMIT 5'),
	(4, 'count by kind, a range of days that cuts the key', 'SELECT kind, count(*) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) AND created_at >= date ''2026-03-01'' GROUP BY 1'),
	(5, 'count by id in a range of days', 'SELECT dim_a_id, count(*) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) AND created_at >= date ''2026-02-01'' AND created_at < date ''2026-04-01'' GROUP BY 1'),
	(6, 'count by score, two keys', 'SELECT score, count(*) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id IN (1, 2)) GROUP BY 1'),
	(7, 'sum by id, mid key', 'SELECT dim_a_id, count(*), sum(amount) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 2) GROUP BY 1'),
	(8, 'count by kind, small key', 'SELECT kind, count(*) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 3) GROUP BY 1'),
	(9, 'count by month and kind, big key', 'SELECT date_trunc(''month'', created_at), kind, count(*) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) GROUP BY 1, 2');

-- the answer and the route: md5 of the rows with the sift plan on and off, and whether the superunits (key groups) answered
CREATE FUNCTION probe(q text) RETURNS TABLE (same boolean, scope_units boolean, supers boolean) LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	on_h text;
	off_h text;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO on_h;
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO off_h;
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN QUERY SELECT on_h = off_h, jsonb_path_exists(p, '$.**."Scope Units Read"'),
		coalesce((jsonb_path_query_first(p, '$.**."Superunits"') #>> '{}')::int, 0) > 0;
END $$;

SELECT c.n, c.label, r.* FROM cases c, LATERAL probe(c.stmt) r ORDER BY c.n;

-- a group that cannot answer is not tried: a filter on two columns (one of them cuts every unit) and a range of days that cuts the group read no more blocks with the groups than without
CREATE FUNCTION probe_blocks(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	hit_on bigint;
	hit_off bigint;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS, FORMAT JSON) ' || q INTO p;
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS, FORMAT JSON) ' || q INTO p;
	hit_on := (jsonb_path_query_first(p, '$.**."Shared Hit Blocks"') #>> '{}')::bigint;
	PERFORM set_config('sift.agg_super', 'off', true);
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS, FORMAT JSON) ' || q INTO p;
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS, FORMAT JSON) ' || q INTO p;
	hit_off := (jsonb_path_query_first(p, '$.**."Shared Hit Blocks"') #>> '{}')::bigint;
	PERFORM set_config('sift.agg_super', 'on', true);
	RETURN hit_on <= hit_off + 1;
END $$;
SELECT probe_blocks('SELECT count(*) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) AND score = 3 AND kind = ''k1''') AS two_columns,
       probe_blocks('SELECT kind, count(*) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) AND created_at >= date ''2026-03-01'' GROUP BY 1') AS day_range,
       probe_blocks('SELECT count(*) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 2) AND score = 3 AND kind = ''k1''') AS two_columns_mid_key;

-- every byte of the block of a small index (1,500 events, groups of 13 units), read as itself with one bit flipped: all refused
CREATE TABLE events_s (id int PRIMARY KEY, dim_a_id int, created_at date, score int, kind text, amount int) WITH (autovacuum_enabled = false);
INSERT INTO events_s SELECT i, 1 + (i * 7919) % 67, date '2026-01-01' + (i / 40), 1 + i % 10, 'k' || (i % 4), i % 500 FROM generate_series(1, 1500) i;
CREATE INDEX events_s_col ON events_s USING sift_col (id, dim_a_id, created_at, score, kind, amount)
	WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
REINDEX INDEX events_s_col;
VACUUM (FREEZE, ANALYZE) events_s;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('events_s_col');
SELECT supers > 0 AS has_blocks, damaged > 0 AS damages_made, refused = damaged AS all_refused, unnoticed = 0 AS none_unnoticed FROM sift._test_col_super_damage('events_s_col');

-- writes since the build, deletes and VACUUM, a promotion and a merge: the groups of the segments written are of the rows that are left
INSERT INTO events SELECT 30000 + i, 1 + (i * 31) % 67, date '2026-05-20' + i % 5, 1 + i % 10, 'k' || (i % 4), i % 500 FROM generate_series(1, 3000) i;
SELECT c.n, r.same, r.scope_units FROM cases c, LATERAL probe(c.stmt) r ORDER BY c.n;
DELETE FROM events WHERE id % 7 = 0;
VACUUM (FREEZE, index_cleanup on, disable_page_skipping) events;
SELECT c.n, r.same, r.scope_units FROM cases c, LATERAL probe(c.stmt) r ORDER BY c.n;
SELECT sift.col_promote('events_col') IS NOT NULL AS promoted;
SELECT sift.col_verify('events_col', true);
SELECT c.n, r.same, r.scope_units FROM cases c, LATERAL probe(c.stmt) r ORDER BY c.n;
SELECT sift.col_merge('events_col', true) IS NOT NULL AS merged;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('events_col');
SELECT sift.col_verify('events_col', true);
INSERT INTO events_s SELECT 5000 + i, 1 + (i * 31) % 67, date '2026-05-20' + i % 5, 1 + i % 10, 'k' || (i % 4), i % 500 FROM generate_series(1, 300) i;
SELECT sift.col_promote('events_s_col') IS NOT NULL AND sift.col_merge('events_s_col', true) IS NOT NULL AS merged_small;
SELECT sift.col_verify('events_s_col', true);
SELECT supers > 0 AS has_blocks, refused = damaged AS all_refused, unnoticed = 0 AS none_unnoticed FROM sift._test_col_super_damage('events_s_col');
SELECT c.n, r.same, r.scope_units, r.supers FROM cases c, LATERAL probe(c.stmt) r ORDER BY c.n;

DROP TABLE events, events_s, tenant_scope, cases;
DROP FUNCTION probe(text), probe_blocks(text);
DROP EXTENSION pg_sift_test CASCADE;
DROP EXTENSION pg_sift;
