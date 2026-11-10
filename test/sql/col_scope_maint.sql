-- The scope segments of an index with a scope live with its entity segments: a promotion writes the scope segments of its rows in the switch of the directory that adds the entity segment,
-- a merge of entity segments keeps the addresses (the entries stay), the scope segments merge by the range of their key under the oldest stamp, VACUUM ends the entries of a dead
-- address in every key, a rewrite fills them again when they are stale or an entity segment lost its flag, and REINDEX builds them anew. After each state the scope read is the plain join
-- (md5), it reads the scope segments (the verdict is "covered", not a fallback), every live row has the entries of its member (no more, no fewer), and col_verify is clean.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;
SET sift.test_scope_trust = on;

-- 12 tenants; 300 members, each in one to six of them (6 for none of the multiples of 6 less); padded events in a random heap order, some without a member or with one that has no pair
CREATE TABLE tenant_scope (dim_a_id int, tenant_id int, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT m, 1 + (m * 5 + j * 7) % 12 FROM generate_series(1, 300) m, generate_series(0, m % 6) j WHERE m % 25 <> 0;
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN 999 ELSE 1 + (i * 7919) % 320 END,
	timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 20000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v)
	WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
VACUUM (FREEZE, ANALYZE) events;
VACUUM (ANALYZE) tenant_scope;

-- the same rows as the plain join, from the scope segments (or why not): the whole statement, per shape and key
CREATE FUNCTION scope_run(q text, plain text, OUT same boolean, OUT stored boolean, OUT why text)
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
END $$;

CREATE TABLE shapes (n int, k int, q text, plain text);
INSERT INTO shapes
SELECT n, k, replace(q, '@', 'sift.in_scope(''events_col''::regclass, ''tenant_id'', e.dim_a_id, ' || k || ')'),
	replace(q, '@', 'e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ' || k || ')')
FROM (VALUES
	(1, 3, 'SELECT count(*), sum(v), min(id), max(id) FROM events e WHERE @'),
	(2, 7, 'SELECT v, count(*) FROM events e WHERE id > 5000 AND @ GROUP BY v ORDER BY v'),
	(3, 11, 'SELECT count(*), sum(v) FROM events e WHERE @ AND created_at >= timestamptz ''2026-03-01'' AND created_at < timestamptz ''2026-04-01'''),
	(4, 5, 'SELECT id, created_at FROM events e WHERE @ ORDER BY created_at DESC, id DESC LIMIT 20'),
	(5, 12, 'SELECT count(*) FROM events e WHERE @')) s(n, k, q);

-- every live row has the entries of its member under the join of the pairs now (the scope relation is not edited in this test, so the stamp is as good as now), and no entry of a live row is another
CREATE FUNCTION entries(OUT missing bigint, OUT extra bigint) LANGUAGE sql AS $$
	SELECT (SELECT count(*) FROM (SELECT e.ctid, s.tenant_id FROM events e JOIN tenant_scope s USING (dim_a_id)
				EXCEPT SELECT tid, key::int FROM sift._test_col_scope_entries('events_col')) m),
		   (SELECT count(*) FROM (SELECT tid, key::int FROM sift._test_col_scope_entries('events_col') WHERE tid IN (SELECT ctid FROM events)
				EXCEPT SELECT e.ctid, s.tenant_id FROM events e JOIN tenant_scope s USING (dim_a_id)) x) $$;

CREATE FUNCTION state(label text, OUT state text, OUT covered boolean, OUT entity_segs bigint, OUT scope_segs bigint, OUT same boolean, OUT stored boolean, OUT why text,
					  OUT missing bigint, OUT extra bigint) LANGUAGE plpgsql AS $$
BEGIN
	state := label;
	covered := sift._test_col_scope_covered('events_col', 'tenant_id');
	SELECT count(DISTINCT s.gen) FILTER (WHERE family = 'entities'), count(DISTINCT s.gen) FILTER (WHERE family = 'scope')
		INTO entity_segs, scope_segs FROM sift._test_col_scope_stamps('events_col') s;
	SELECT bool_and(r.same), bool_and(r.stored), min(r.why) INTO same, stored, why FROM shapes s, LATERAL scope_run(s.q, s.plain) r;
	SELECT e.missing, e.extra INTO missing, extra FROM entries() e;
END $$;

-- built
SELECT * FROM state('built');
SELECT sift.col_verify('events_col', true);

-- rows in the write buffer have no entries: the entity path reads them with the members of the key; the segments still cover the rows they hold
INSERT INTO events SELECT 20000 + i, 1 + i % 300, timestamptz '2026-02-01' + i * interval '1 hour', i % 10, 'y' FROM generate_series(1, 400) i;
SELECT state, covered, entity_segs, scope_segs, same, stored, why FROM state('write buffer');

-- promoted: the entity segment and the scope segments of the rows come in one switch of the directory (one more of each); the rows of the buffer have their entries
SELECT sift.col_promote('events_col') >= 0 AS promoted;
SELECT * FROM state('promoted');
INSERT INTO events SELECT 21000 + i, 1 + i % 300, timestamptz '2026-04-01' + i * interval '1 hour', i % 10, 'z' FROM generate_series(1, 300) i;
SELECT sift.col_promote('events_col') >= 0 AS promoted_again;
SELECT * FROM state('promoted twice');
SELECT sift.col_verify('events_col', true);

-- the stamps: a promotion stamps its segments with the snapshot it read the pairs under; the merge of the scope segments keeps the oldest
CREATE TEMP TABLE stamps_before AS SELECT DISTINCT family, scope, lineage, horizon FROM sift._test_col_scope_stamps('events_col');
SELECT family, count(DISTINCT horizon) AS horizons FROM sift._test_col_scope_stamps('events_col') WHERE scope = 0 GROUP BY family ORDER BY family;

-- updates: the new versions are in the buffer (entity path), the old addresses keep their entries until VACUUM
UPDATE events SET v = v + 1 WHERE id % 7 = 0;
SELECT state, covered, same, stored, why FROM state('updated');
SELECT sift.col_promote('events_col') >= 0 AS promoted_updates;
SELECT * FROM state('updates promoted');

-- the merge of everything: the entity segments into one (the entries stay, the addresses are the same), the scope segments into one by the range of their key, under the oldest stamp
SELECT sift.col_merge('events_col', true) >= 0 AS merged;
SELECT * FROM state('merged');
SELECT family, count(DISTINCT gen) AS segments, count(DISTINCT horizon) AS horizons, bool_and(covered) FILTER (WHERE family = 'entities') AS covered FROM sift._test_col_scope_stamps('events_col') WHERE scope = 0 GROUP BY family ORDER BY family;
SELECT (SELECT min(horizon) FROM sift._test_col_scope_stamps('events_col') WHERE family = 'scope' AND scope = 0) =
	   (SELECT min(horizon) FROM stamps_before WHERE family = 'scope' AND scope = 0) AS oldest_stamp_kept;
SELECT sift.col_merge('events_col', true) >= 0 AS merged_again;
SELECT * FROM state('merged twice');

-- deletes and updates ended by VACUUM: no entry of a dead address is left in any key; the entries of the others are as they were; the stamps stay
DELETE FROM events WHERE id % 5 = 0 OR id > 20300;
CREATE TEMP TABLE stamps_vac AS SELECT family, scope, lineage, horizon FROM sift._test_col_scope_stamps('events_col');
SELECT state, covered, same, stored, why FROM state('deleted');
VACUUM events;
SELECT * FROM state('vacuumed');
SELECT count(*) AS entries_of_dead_addresses FROM sift._test_col_scope_entries('events_col') x WHERE NOT EXISTS (SELECT 1 FROM events e WHERE e.ctid = x.tid);
SELECT (SELECT count(*) FROM (SELECT * FROM stamps_vac EXCEPT SELECT family, scope, lineage, horizon FROM sift._test_col_scope_stamps('events_col')) s) AS stamps_changed_by_vacuum;
SELECT sift.col_verify('events_col', true);
-- ... and the rewrite after it (the dead rows of the scope segments are gone from them)
SELECT sift.col_merge('events_col', true) >= 0 AS merged_after_vacuum;
SELECT * FROM state('merged after vacuum');
SELECT sift.col_verify('events_col', true);

-- a rewrite fills the entries again when they are stale (until the journal says so, a test switch does) and when an entity segment lost its flag (a merge of a covered segment with one
-- that was not): the read falls back meanwhile, exact, and the rewrite covers
SELECT sift._test_col_scope_flag('events_col', false);
SELECT state, covered, same, stored, why FROM state('flag lost');
SELECT sift.col_maintain('events_col') AS rewritten;
SELECT * FROM state('refilled');
SET sift.test_scope_stale = on;
SELECT sift.col_maintain('events_col') AS rewritten_stale;
RESET sift.test_scope_stale;
SELECT * FROM state('refilled stale');
SELECT (SELECT min(horizon) FROM sift._test_col_scope_stamps('events_col') WHERE family = 'scope' AND scope = 0) >
	   (SELECT min(horizon) FROM stamps_before WHERE family = 'scope' AND scope = 0) AS stamp_is_new;
SELECT sift.col_verify('events_col', true);

-- REINDEX builds the scope segments anew
REINDEX INDEX events_col;
SELECT * FROM state('reindexed');
SELECT sift.col_verify('events_col', true);

DROP TABLE shapes, events, tenant_scope, stamps_before, stamps_vac;
DROP FUNCTION state(text), entries(), scope_run(text, text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
