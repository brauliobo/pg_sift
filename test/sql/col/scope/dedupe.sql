-- A fact read for several scope keys at once with the meaning of IN / EXISTS counts once, though its member has pairs with several of the keys, and the join counts it once for each pair. An
-- index of two scopes, members in up to six keys: every statement (IN, EXISTS, a top-N over several keys, a join to the relation of the other scope) is md5 equal through the scope node, through
-- sift with sift.agg_scope off and through the normal plan, and says how the node read it.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
-- members in up to six keys on purpose: above the default limit of the multiplicity
SET jit = off;
SET client_min_messages = warning;
-- the scope summary is Format 2.0; the testing library builds new indexes in Format 1.0 when told to
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

-- 12 tenants and 20 groups; 300 members in one to six tenants (a quarter of them in two or more) and in zero to four groups
CREATE TABLE tenant_scope (dim_a_id int, tenant_id int, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT m, 1 + (m * 5 + j * 7) % 12 FROM generate_series(1, 300) m, generate_series(0, m % 6) j WHERE m % 25 <> 0;
CREATE TABLE group_scope (dim_a_id int, group_id int, PRIMARY KEY (dim_a_id, group_id));
INSERT INTO group_scope SELECT m, 1 + (m * 3 + j * 5) % 20 FROM generate_series(1, 300) m, generate_series(0, m % 4) j WHERE m % 17 <> 0;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX group_scope_journal ON group_scope USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN 999 ELSE 1 + (i * 7919) % 320 END,
	timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 20000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v)
	WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) max_multiplicity(8); group_id: dim_a_id -> group_scope(dim_a_id, group_id) max_multiplicity(8)');
REINDEX INDEX events_col;
VACUUM (FREEZE, ANALYZE) events;

-- the same rows through every plan, and the Scope Read of the node (none: not a scope read)
CREATE FUNCTION scope_run(q text, OUT same boolean, OUT route text)
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
	route := coalesce(jsonb_path_query_first(p, '$.**."Scope Read"') #>> '{}', 'none');
END $$;

CREATE TABLE sets (n int, keys int8[]);
INSERT INTO sets VALUES (1, '{2,9}'), (2, '{1,2,3,4,5,6}'), (3, '{7,9,11}'), (4, '{12,13}'), (5, '{5}'), (6, '{9,9,2}'), (7, '{}');

-- once: IN and EXISTS over several keys, several classes
CREATE FUNCTION once_phase() RETURNS TABLE (n int, class text, same boolean, route text) LANGUAGE sql AS $$
	SELECT s.n, c.class, r.same, r.route
	FROM sets s,
	LATERAL (VALUES
		('count', 'SELECT count(*), sum(v), min(id), max(id) FROM events e WHERE @', 'SELECT count(*), sum(v), min(id), max(id) FROM events e WHERE EXISTS (SELECT 1 FROM tenant_scope t WHERE t.dim_a_id = e.dim_a_id AND t.tenant_id = ANY (''' || s.keys::text || '''::int8[]))'),
		('groups', 'SELECT v, count(*) FROM events e WHERE id > 5000 AND @ GROUP BY v ORDER BY v', 'SELECT v, count(*) FROM events e WHERE id > 5000 AND e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ANY (''' || s.keys::text || '''::int8[])) GROUP BY v ORDER BY v'),
		('window', 'SELECT count(*), sum(v) FROM events e WHERE @ AND created_at >= timestamptz ''2026-03-01'' AND created_at < timestamptz ''2026-04-01''', 'SELECT count(*), sum(v) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ANY (''' || s.keys::text || '''::int8[])) AND created_at >= timestamptz ''2026-03-01'' AND created_at < timestamptz ''2026-04-01'''),
		('top20', 'SELECT id, created_at FROM events e WHERE @ ORDER BY created_at DESC, id DESC LIMIT 20', 'SELECT id, created_at FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ANY (''' || s.keys::text || '''::int8[])) ORDER BY created_at DESC, id DESC LIMIT 20')) c(class, q, plain),
	LATERAL scope_run(c.plain) r
	ORDER BY s.n, c.class
$$;
SELECT * FROM once_phase();

-- the join counts the members of two keys twice, IN once
SELECT (SELECT count(*) FROM events e JOIN tenant_scope t ON t.dim_a_id = e.dim_a_id WHERE t.tenant_id IN (2, 9)) AS naive_count,
	   (SELECT count(*) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id IN (2, 9))) AS once_count;

-- a join to the relation of the other scope, by the keys of the first: the facts of the tenants once for each live group of their member
CREATE FUNCTION join_phase() RETURNS TABLE (n int, class text, same boolean, route text) LANGUAGE sql AS $$
	SELECT s.n, c.class, r.same, r.route
	FROM sets s,
	LATERAL (VALUES
		('group_join', 'SELECT g.group_id, count(*), sum(e.v) FROM events e JOIN group_scope g ON g.dim_a_id = e.dim_a_id WHERE @ GROUP BY 1 ORDER BY 1',
		 'SELECT g.group_id, count(*), sum(e.v) FROM events e JOIN group_scope g ON g.dim_a_id = e.dim_a_id WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ANY (''' || s.keys::text || '''::int8[])) GROUP BY 1 ORDER BY 1'),
		('group_join_month', 'SELECT g.group_id, date_trunc(''month'', e.created_at) m, count(*) FROM events e JOIN group_scope g ON g.dim_a_id = e.dim_a_id WHERE @ AND e.v > 3 GROUP BY 1, 2 ORDER BY 1, 2',
		 'SELECT g.group_id, date_trunc(''month'', e.created_at) m, count(*) FROM events e JOIN group_scope g ON g.dim_a_id = e.dim_a_id WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ANY (''' || s.keys::text || '''::int8[])) AND e.v > 3 GROUP BY 1, 2 ORDER BY 1, 2')) c(class, q, plain),
	LATERAL scope_run(c.plain) r
	WHERE s.n IN (1, 2, 5)
	ORDER BY s.n, c.class
$$;
SELECT * FROM join_phase();

-- the same after a promotion, deletes and a VACUUM, and a merge
INSERT INTO events SELECT 20000 + i, 1 + i % 300, timestamptz '2026-02-01' + i * interval '1 hour', i % 10, 'y' FROM generate_series(1, 400) i;
SELECT sift.col_promote('events_col') >= 0 AS promoted;
DELETE FROM events WHERE id % 7 = 0;
VACUUM events;
SELECT sift.col_merge('events_col', true) >= 0 AS merged;
SELECT n, class, same, route FROM once_phase() WHERE class IN ('count', 'top20');
SELECT n, class, same, route FROM join_phase() WHERE class = 'group_join';
SELECT sift.col_verify('events_col', true);

DROP TABLE sets, events, tenant_scope, group_scope;
DROP FUNCTION once_phase(), join_phase(), scope_run(text);
DROP EXTENSION pg_sift;
