-- A fact read for several scope keys at once with the meaning of IN / EXISTS counts once, though its member has pairs with several of the keys: the scope units of each key hold it, and the
-- entry of the unit of the first key of the member is the one that counts (the others are replicas), so the answer is the plain join's (md5) and a naive IN over the replicated units
-- would count the members of two keys twice. A top-N over several keys is read from the entity rows (the units of different keys have no order across them), and a join to another
-- relation per fact (GROUP BY the key of another scope) goes on top of the scan of the scope units of one key: the scope units expose the same rows as the entity scan.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;
SET sift.test_scope_trust = on;

-- 12 tenants and 20 groups; 300 members in one to six tenants (a quarter of them in two or more) and in zero to four groups
CREATE TABLE tenant_scope (dim_a_id int, tenant_id int, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT m, 1 + (m * 5 + j * 7) % 12 FROM generate_series(1, 300) m, generate_series(0, m % 6) j WHERE m % 25 <> 0;
CREATE TABLE group_scope (dim_a_id int, group_id int, PRIMARY KEY (dim_a_id, group_id));
INSERT INTO group_scope SELECT m, 1 + (m * 3 + j * 5) % 20 FROM generate_series(1, 300) m, generate_series(0, m % 4) j WHERE m % 17 <> 0;
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN 999 ELSE 1 + (i * 7919) % 320 END,
	timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 20000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v)
	WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id); group_id: dim_a_id -> group_scope(dim_a_id, group_id)');
VACUUM (FREEZE, ANALYZE) events;

-- the same rows as the plain statement, whether the scope units were read, why not, whether the facts of a replica are left to the first key, and the scope the statement reads
CREATE FUNCTION scope_run(q text, plain text, OUT same boolean, OUT stored boolean, OUT why text, OUT dedupe boolean, OUT scopes_read bigint)
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
	dedupe := jsonb_path_exists(p, '$.**."Scope Dedupe"');
	scopes_read := (SELECT count(*) FROM jsonb_path_query(p, '$.**."Scope Index"'));
END $$;

CREATE TABLE sets (n int, keys int8[]);
INSERT INTO sets VALUES (1, '{2,9}'), (2, '{1,2,3,4,5,6}'), (3, '{7,9,11}'), (4, '{12,13}'), (5, '{5}'), (6, '{9,9,2}'), (7, '{}');

-- once: IN and EXISTS over several keys, several classes
CREATE FUNCTION once_phase() RETURNS TABLE (n int, class text, same boolean, stored boolean, why text, dedupe boolean) LANGUAGE sql AS $$
	SELECT s.n, c.class, r.same, r.stored, r.why, r.dedupe
	FROM sets s,
	LATERAL (VALUES
		('count', 'SELECT count(*), sum(v), min(id), max(id) FROM events e WHERE @', 'SELECT count(*), sum(v), min(id), max(id) FROM events e WHERE EXISTS (SELECT 1 FROM tenant_scope t WHERE t.dim_a_id = e.dim_a_id AND t.tenant_id = ANY (''' || s.keys::text || '''::int8[]))'),
		('groups', 'SELECT v, count(*) FROM events e WHERE id > 5000 AND @ GROUP BY v ORDER BY v', 'SELECT v, count(*) FROM events e WHERE id > 5000 AND e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ANY (''' || s.keys::text || '''::int8[])) GROUP BY v ORDER BY v'),
		('window', 'SELECT count(*), sum(v) FROM events e WHERE @ AND created_at >= timestamptz ''2026-03-01'' AND created_at < timestamptz ''2026-04-01''', 'SELECT count(*), sum(v) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ANY (''' || s.keys::text || '''::int8[])) AND created_at >= timestamptz ''2026-03-01'' AND created_at < timestamptz ''2026-04-01'''),
		('top20', 'SELECT id, created_at FROM events e WHERE @ ORDER BY created_at DESC, id DESC LIMIT 20', 'SELECT id, created_at FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ANY (''' || s.keys::text || '''::int8[])) ORDER BY created_at DESC, id DESC LIMIT 20')) c(class, q, plain),
	LATERAL scope_run(replace(c.q, '@', 'sift.in_scope(''events_col''::regclass, ''tenant_id'', e.dim_a_id, ''' || s.keys::text || '''::int8[])'), c.plain) r
	ORDER BY s.n, c.class
$$;
SELECT * FROM once_phase();

-- a naive read of the replicated units counts the members of two keys twice: what the dedupe leaves out
SELECT (SELECT count(*) FROM events e JOIN tenant_scope t ON t.dim_a_id = e.dim_a_id WHERE t.tenant_id IN (2, 9)) AS naive_count,
	   (SELECT count(*) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id IN (2, 9))) AS once_count;
SELECT count(*) AS scope_read_count FROM events e WHERE sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, '{2,9}'::int8[]);

-- per pair (the join and GROUP BY the key): a fact counts once for each pair, which the unit of each key gives, and a join on top of the scan of the scope units of the tenants gives the
-- rows of the groups of each fact: the facts of the tenant once for each live group of their member (md5 against the plain join); the group scope is not read as units there
CREATE FUNCTION join_phase() RETURNS TABLE (n int, class text, same boolean, stored boolean, why text, scopes_read bigint) LANGUAGE sql AS $$
	SELECT s.n, c.class, r.same, r.stored, r.why, r.scopes_read
	FROM sets s,
	LATERAL (VALUES
		('group_join', 'SELECT g.group_id, count(*), sum(e.v) FROM events e JOIN group_scope g ON g.dim_a_id = e.dim_a_id WHERE @ GROUP BY 1 ORDER BY 1',
		 'SELECT g.group_id, count(*), sum(e.v) FROM events e JOIN group_scope g ON g.dim_a_id = e.dim_a_id WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ANY (''' || s.keys::text || '''::int8[])) GROUP BY 1 ORDER BY 1'),
		('group_join_month', 'SELECT g.group_id, date_trunc(''month'', e.created_at) m, count(*) FROM events e JOIN group_scope g ON g.dim_a_id = e.dim_a_id WHERE @ AND e.v > 3 GROUP BY 1, 2 ORDER BY 1, 2',
		 'SELECT g.group_id, date_trunc(''month'', e.created_at) m, count(*) FROM events e JOIN group_scope g ON g.dim_a_id = e.dim_a_id WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ANY (''' || s.keys::text || '''::int8[])) AND e.v > 3 GROUP BY 1, 2 ORDER BY 1, 2')) c(class, q, plain),
	LATERAL scope_run(replace(c.q, '@', 'sift.in_scope(''events_col''::regclass, ''tenant_id'', e.dim_a_id, ''' || s.keys::text || '''::int8[])'), c.plain) r
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
SELECT n, class, same, stored, why, dedupe FROM once_phase() WHERE class IN ('count', 'top20');
SELECT n, class, same, stored, why, scopes_read FROM join_phase() WHERE class = 'group_join';
SELECT sift.col_verify('events_col', true);

DROP TABLE sets, events, tenant_scope, group_scope;
DROP FUNCTION once_phase(), join_phase(), scope_run(text, text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
