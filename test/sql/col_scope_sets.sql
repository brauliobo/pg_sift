-- The id sets by calendar month of the key groups (column format minor 51, docs/COL-SCOPE.md "Id sets by month"): the units that hold one scope key are a group that keeps, for each month of its
-- rows, the ids of its 32-bit integer dims, so count(DISTINCT id) by month over a key reads one set for each month and group instead of the rows of every unit. The answer is md5 equal to the
-- plan with sift off, a month that a test cuts, a key through a zone that skips a day, a sum, a text leaf and the ids of changed pairs are read from the units, the block is checked against the
-- units and every byte of it is refused when damaged, and it follows writes, deletes, VACUUM, promotion and merge. Counts and hashes only.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.test_col_unit_rows = 100;
SET TimeZone = 'UTC';

-- key 1 holds ids 1 to 60, key 2 ids 61 to 66, key 3 id 67 only
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
SELECT sift.col_verify('events_col', true);

CREATE TABLE cases (n int, label text, stmt text);
INSERT INTO cases VALUES
	(1, 'distinct ids by month, big key', 'SELECT date_trunc(''month'', created_at) m, count(DISTINCT dim_a_id) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) GROUP BY 1'),
	(2, 'rows and distinct ids by month, big key', 'SELECT date_trunc(''month'', created_at) m, count(*), count(DISTINCT dim_a_id) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) GROUP BY 1'),
	(3, 'distinct ids by month from a month on', 'SELECT date_trunc(''month'', created_at) m, count(DISTINCT dim_a_id) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) AND created_at >= date ''2026-03-01'' GROUP BY 1'),
	(4, 'distinct ids by month from a day inside a month', 'SELECT date_trunc(''month'', created_at) m, count(DISTINCT dim_a_id) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) AND created_at >= date ''2026-03-15'' GROUP BY 1'),
	(5, 'distinct ids by year', 'SELECT date_trunc(''year'', created_at) y, count(DISTINCT dim_a_id) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) GROUP BY 1'),
	(6, 'two distinct dims by month', 'SELECT date_trunc(''month'', created_at) m, count(DISTINCT score), count(DISTINCT dim_a_id) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) GROUP BY 1'),
	(7, 'distinct ids by month, two keys', 'SELECT date_trunc(''month'', created_at) m, count(DISTINCT dim_a_id) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id IN (1, 2)) GROUP BY 1'),
	(8, 'distinct ids by month, mid key', 'SELECT date_trunc(''month'', created_at) m, count(DISTINCT dim_a_id) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 2) GROUP BY 1'),
	(9, 'distinct ids by month, small key', 'SELECT date_trunc(''month'', created_at) m, count(DISTINCT dim_a_id) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 3) GROUP BY 1'),
	(10, 'distinct ids by month and a sum', 'SELECT date_trunc(''month'', created_at) m, count(DISTINCT dim_a_id), sum(amount) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) GROUP BY 1'),
	(11, 'distinct kinds by month (a text leaf)', 'SELECT date_trunc(''month'', created_at) m, count(DISTINCT kind) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) GROUP BY 1'),
	(12, 'distinct ids with a filter on another column', 'SELECT date_trunc(''month'', created_at) m, count(DISTINCT dim_a_id) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) AND score = 3 GROUP BY 1'),
	(13, 'distinct ids, no month', 'SELECT count(DISTINCT dim_a_id) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) AND created_at >= date ''2026-03-01'''),
	(14, 'distinct ids by day', 'SELECT created_at, count(DISTINCT dim_a_id) FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 3) GROUP BY 1');

-- the answer and the route: md5 of the rows with the sift plan on and off, and whether key groups (superunits) answered
CREATE FUNCTION probe(q text) RETURNS TABLE (same boolean, supers boolean) LANGUAGE plpgsql AS $$
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
	RETURN QUERY SELECT on_h = off_h, coalesce((jsonb_path_query_first(p, '$.**."Superunits"') #>> '{}')::int, 0) > 0;
END $$;
SELECT c.n, c.label, r.* FROM cases c, LATERAL probe(c.stmt) r ORDER BY c.n;

-- the groups read fewer blocks than the units: the distinct ids by month of the big key
CREATE FUNCTION probe_blocks(q text, slack int DEFAULT 1) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	hit_on bigint;
	hit_off bigint;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS, FORMAT JSON) ' || q INTO p;
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS, FORMAT JSON) ' || q INTO p;
	hit_on := (jsonb_path_query_first(p, '$.**."Shared Hit Blocks"') #>> '{}')::bigint;
	PERFORM set_config('sift.test_agg_super', 'off', true);
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS, FORMAT JSON) ' || q INTO p;
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS, FORMAT JSON) ' || q INTO p;
	hit_off := (jsonb_path_query_first(p, '$.**."Shared Hit Blocks"') #>> '{}')::bigint;
	PERFORM set_config('sift.test_agg_super', 'on', true);
	RETURN hit_on <= hit_off + slack;
END $$;
SELECT probe_blocks(stmt, -20) AS fewer_blocks FROM cases WHERE n IN (1, 2);

-- a zone that skips the last day of a month (Pacific/Kiritimati had no 31 December 1994): the key of that day is the month after, so the days of a month give two keys and the group is not read
CREATE TABLE events_z (id int PRIMARY KEY, dim_a_id int, created_at date, score int) WITH (autovacuum_enabled = false);
INSERT INTO events_z SELECT i, 1 + (i * 7919) % 67, date '1994-12-01' + (i / 30) % 62, 1 + i % 10 FROM generate_series(1, 6000) i;
CREATE INDEX events_z_col ON events_z USING sift_col (id, dim_a_id, created_at, score)
	WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
REINDEX INDEX events_z_col;
VACUUM (FREEZE, ANALYZE) events_z;
SELECT sift.col_verify('events_z_col', true);
CREATE TABLE cases_z (n int, zone text, stmt text);
INSERT INTO cases_z SELECT z.n, z.zone, 'SELECT date_trunc(''month'', created_at) m, count(DISTINCT dim_a_id) FROM events_z WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) GROUP BY 1'
	FROM (VALUES (1, 'UTC'), (2, 'Pacific/Kiritimati'), (3, 'America/Sao_Paulo'), (4, 'Asia/Tokyo')) z (n, zone);
CREATE FUNCTION probe_zone(zone text, q text) RETURNS TABLE (same boolean, supers boolean) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('TimeZone', zone, true);
	RETURN QUERY SELECT * FROM probe(q);
END $$;
SELECT c.n, c.zone, r.* FROM cases_z c, LATERAL probe_zone(c.zone, c.stmt) r ORDER BY c.n;

-- the ids of changed pairs: a member that left or joined a key since the build is read from the units, never from the sets
BEGIN;
DELETE FROM tenant_scope WHERE dim_a_id IN (5, 6, 7) AND tenant_id = 1;
INSERT INTO tenant_scope VALUES (5, 2), (68, 1);
SELECT c.n, r.same FROM cases c, LATERAL probe(c.stmt) r WHERE c.n IN (1, 7, 8) ORDER BY c.n;
ROLLBACK;

-- every byte of the block of a small index (1,500 events, groups of 13 units), read as itself with one bit flipped: all refused
CREATE TABLE events_s (id int PRIMARY KEY, dim_a_id int, created_at date, score int, kind text, amount int) WITH (autovacuum_enabled = false);
INSERT INTO events_s SELECT i, 1 + (i * 7919) % 67, date '2026-01-01' + (i / 40), 1 + i % 10, 'k' || (i % 4), i % 500 FROM generate_series(1, 1500) i;
CREATE INDEX events_s_col ON events_s USING sift_col (id, dim_a_id, created_at, score, kind, amount)
	WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
REINDEX INDEX events_s_col;
VACUUM (FREEZE, ANALYZE) events_s;
SELECT sift.col_verify('events_s_col', true);
SELECT supers AS small_index_answers FROM probe('SELECT date_trunc(''month'', created_at) m, count(DISTINCT dim_a_id) FROM events_s WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) GROUP BY 1');
SELECT supers > 0 AS has_blocks, damaged > 0 AS damages_made, refused = damaged AS all_refused, unnoticed = 0 AS none_unnoticed FROM sift._test_col_super_damage('events_s_col');

-- the same with the rollup of the rows (groups of 4 units) next to the sets
CREATE TABLE events_rs (id int PRIMARY KEY, dim_a_id int, created_at date, score int, kind text, amount int) WITH (autovacuum_enabled = false);
INSERT INTO events_rs SELECT i, 1 + (i * 7919) % 67, date '2026-01-01' + (i / 40), 1 + i % 10, 'k' || (i % 4), i % 500 FROM generate_series(1, 1500) i;
CREATE INDEX events_rs_col ON events_rs USING sift_col (id, dim_a_id, created_at, score, kind, amount)
	WITH (rollup_columns = 'score, kind: amount', superunit = 4, scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
REINDEX INDEX events_rs_col;
VACUUM (FREEZE, ANALYZE) events_rs;
SELECT sift.col_verify('events_rs_col', true);
SELECT supers AS small_rollup_index_answers FROM probe('SELECT date_trunc(''month'', created_at) m, count(DISTINCT dim_a_id) FROM events_rs WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) GROUP BY 1');
SELECT supers > 0 AS has_blocks, damaged > 0 AS damages_made, refused = damaged AS all_refused, unnoticed = 0 AS none_unnoticed FROM sift._test_col_super_damage('events_rs_col');

-- writes since the build, deletes and VACUUM, a promotion and a merge: the groups of the segments written are of the rows that are left
INSERT INTO events SELECT 30000 + i, 1 + (i * 31) % 67, date '2026-05-20' + i % 5, 1 + i % 10, 'k' || (i % 4), i % 500 FROM generate_series(1, 3000) i;
SELECT c.n, r.same FROM cases c, LATERAL probe(c.stmt) r ORDER BY c.n;
DELETE FROM events WHERE id % 7 = 0;
VACUUM (FREEZE, index_cleanup on, disable_page_skipping) events;
SELECT c.n, r.same FROM cases c, LATERAL probe(c.stmt) r ORDER BY c.n;
SELECT sift.col_promote('events_col') IS NOT NULL AS promoted;
SELECT sift.col_verify('events_col', true);
SELECT c.n, r.same FROM cases c, LATERAL probe(c.stmt) r ORDER BY c.n;
SELECT sift.col_merge('events_col', true) IS NOT NULL AS merged;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('events_col');
SELECT sift.col_verify('events_col', true);
SELECT c.n, r.same, r.supers FROM cases c, LATERAL probe(c.stmt) r ORDER BY c.n;

DROP TABLE events, events_z, events_s, events_rs, tenant_scope, cases, cases_z;
DROP FUNCTION probe(text), probe_blocks(text, int), probe_zone(text, text);
DROP EXTENSION pg_sift_test CASCADE;
DROP EXTENSION pg_sift;
