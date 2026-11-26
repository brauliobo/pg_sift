-- Edits of the membership of a scope index (INTERFACE section 6b d and e): an entity that moves between keys (a pair deleted and another inserted, or the key of a pair updated), gains a
-- sixth key, loses its last one, an entity that comes back, a key emptied and filled again, a rolled back edit, the facts of a moved entity deleted or moved to another entity. After every edit
-- a battery of reads (one key, sets of keys counted once, per pair, count/sum/top-N) is the plain join through the scope node, through sift with sift.agg_scope off and through the normal plan.
-- The journal of the pairs says which entities changed: the scope summary takes them as the changes since it (Scope Cube: delta), never stamps taken on trust.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
-- members in up to six keys on purpose: above the default limit of the multiplicity
SET jit = off;
SET client_min_messages = warning;
-- the scope summary is Format 2.0; the testing library builds new indexes in Format 1.0 when told to
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
\ir test/common/sql/col/scope/base.sql

-- 12 keys, 300 entities in 1..6 keys (entity m: 1 + m % 6), entities 100..109 in none, 8,000 padded facts (some without an entity, some of an entity that is nowhere)
CREATE TABLE tenant_scope (dim_a_id int, tenant_id int, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT m, 1 + (m * 5 + j * 7) % 12 FROM generate_series(1, 300) m, generate_series(0, m % 6) j WHERE m NOT BETWEEN 100 AND 109 ON CONFLICT DO NOTHING;
-- the journal of the pairs (INTERFACE section 2): an index of the scope relation that tracks its changes, made before the scope index that reads it
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN 999 ELSE 1 + (i * 7919) % 320 END,
	timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 8000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v)
	WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) max_multiplicity(8)');
REINDEX INDEX events_col;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (ANALYZE) tenant_scope;
SELECT keys, count(*) AS dim_a FROM (SELECT dim_a_id, count(*) AS keys FROM tenant_scope GROUP BY 1) k GROUP BY 1 ORDER BY 1;


-- the battery: the statements of an application; one key is the join (@ the join, $ its condition), a set of keys is EXISTS (a fact once)
CREATE TABLE preds (n int, class text, keys int8[], joined text, cond text);
INSERT INTO preds
SELECT row_number() OVER (), class, keys,
	CASE WHEN class = 'one key' THEN ' JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id' ELSE '' END,
	CASE WHEN class = 'one key' THEN format('s.tenant_id = %s', keys[1]) ELSE format('EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = e.dim_a_id AND s.tenant_id = ANY (%L::int8[]))', keys) END
FROM (VALUES ('one key', '{1}'::int8[]), ('one key', '{4}'), ('one key', '{7}'), ('one key', '{13}'),
	('set', '{2,9}'), ('set', '{1,2,3,4,5,6}'), ('set', '{7,9,11}'), ('set', '{12,13}'), ('set', '{5}'), ('set', '{}')) v(class, keys);
CREATE TABLE shapes (n int, q text);
INSERT INTO shapes VALUES
	(1, 'SELECT count(*), sum(v), min(id), max(id) FROM events e@ WHERE $'),
	(2, 'SELECT v, count(*) FROM events e@ WHERE $ GROUP BY v ORDER BY v'),
	(3, 'SELECT id, created_at FROM events e@ WHERE $ ORDER BY created_at DESC, id DESC LIMIT 10');
CREATE TABLE battery_q AS
SELECT p.class || ' ' || s.n AS name, p.class, p.keys, replace(replace(s.q, '@', p.joined), '$', p.cond) AS q
FROM preds p, shapes s WHERE p.class = 'one key' OR s.n < 3
UNION ALL
-- per pair: a fact once for each pair of its entity, the join counts it so, one key at a time
SELECT 'per pair', 'per pair', NULL,
	'SELECT k, (SELECT count(*) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = k), (SELECT sum(v) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = k) FROM generate_series(1, 13) k ORDER BY k'
UNION ALL
SELECT 'join group by', 'per pair', NULL, q
FROM (VALUES ('SELECT s.tenant_id, count(*), sum(v) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id GROUP BY 1 ORDER BY 1'),
	('SELECT count(*), sum(v) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id IN (2, 9)'),
	('SELECT count(*), sum(v) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id IN (2, 9))')) j(q);

-- the whole battery under the state of the pairs
-- same: every read equals the plain join; scope_one_key: every read of a key that has pairs is a scope read; delta: some read took the changes since the summary
CREATE FUNCTION battery(OUT same boolean, OUT scope_one_key boolean, OUT delta boolean) LANGUAGE plpgsql AS $$
BEGIN
	SELECT bool_and(r.same), bool_and(r.route IS NOT NULL) FILTER (WHERE b.name = 'one key 1' AND b.keys[1] <= 12), bool_or(r.cube = 'delta')
	INTO same, scope_one_key, delta
	FROM battery_q b, LATERAL scope_read(b.q) r;
END $$;

-- the edits (script: statements run in order; abort: the edit is read inside its transaction and rolled back; rebuild: the scope summary is written again from the pairs as they are)
CREATE TABLE edits (n int PRIMARY KEY, descr text, script text, abort boolean DEFAULT false, rebuild boolean DEFAULT false, patch boolean DEFAULT false);
INSERT INTO edits (n, descr, script, abort, rebuild, patch) VALUES
(0, 'as built', 'SELECT 1', false, true, false),
(1, 'an entity of 3 keys moves one pair to a free key',
 'WITH m AS (SELECT dim_a_id FROM tenant_scope GROUP BY 1 HAVING count(*) = 3 ORDER BY 1 LIMIT 1),
	d AS (DELETE FROM tenant_scope t USING m WHERE t.dim_a_id = m.dim_a_id AND t.tenant_id = (SELECT min(tenant_id) FROM tenant_scope WHERE dim_a_id = m.dim_a_id) RETURNING t.dim_a_id)
  INSERT INTO tenant_scope SELECT m.dim_a_id, k FROM m, generate_series(1, 12) k WHERE NOT EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = m.dim_a_id AND s.tenant_id = k) ORDER BY k DESC LIMIT 1',
 false, false, true),
(2, 'the key of the pairs of 30 dim_a is updated (the key changes, the entity stays)',
 'UPDATE tenant_scope t SET tenant_id = 1 + t.tenant_id % 12 WHERE dim_a_id % 10 = 3 AND NOT EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = t.dim_a_id AND s.tenant_id = 1 + t.tenant_id % 12)',
 false, false, true),
(3, 'an entity of 5 keys gains a sixth, one of 6 keys loses two, one of 1 key loses it',
 'INSERT INTO tenant_scope SELECT m, k FROM (SELECT dim_a_id AS m FROM tenant_scope GROUP BY 1 HAVING count(*) = 5 ORDER BY 1 LIMIT 3) a, generate_series(1, 12) k
	WHERE NOT EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = m AND s.tenant_id = k) AND k = (SELECT min(k2) FROM generate_series(1, 12) k2 WHERE NOT EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = m AND s.tenant_id = k2));
  DELETE FROM tenant_scope WHERE (dim_a_id, tenant_id) IN (SELECT dim_a_id, min(tenant_id) FROM tenant_scope WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope GROUP BY 1 HAVING count(*) = 6 ORDER BY 1 LIMIT 5) GROUP BY 1);
  DELETE FROM tenant_scope WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope GROUP BY 1 HAVING count(*) = 1 ORDER BY 1 LIMIT 5)',
 false, false, true),
(4, 'dim_a that had no pair get pairs in three keys',
 'INSERT INTO tenant_scope SELECT m, k FROM generate_series(100, 109) m, generate_series(1, 3) j, LATERAL (SELECT 1 + (m + j * 5) % 12 AS k) x ON CONFLICT DO NOTHING', false, false, true),
(5, 'a key is emptied', 'CREATE TEMP TABLE saved AS SELECT * FROM tenant_scope WHERE tenant_id = 7; DELETE FROM tenant_scope WHERE tenant_id = 7', false, false, true),
(6, 'the key is filled again with the same pairs', 'INSERT INTO tenant_scope SELECT * FROM saved', false, false, true),
(7, 'a rolled back edit: a pair moved, a pair added and a key emptied inside one transaction (read inside it, then rolled back)',
 'UPDATE tenant_scope t SET tenant_id = 1 + t.tenant_id % 12 WHERE dim_a_id % 5 = 1 AND NOT EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = t.dim_a_id AND s.tenant_id = 1 + t.tenant_id % 12);
  DELETE FROM tenant_scope WHERE tenant_id = 3;
  INSERT INTO tenant_scope VALUES (1, 13), (2, 13), (301, 13) ON CONFLICT DO NOTHING', true, false, false),
(8, 'an entity moves between keys in the transaction that deletes its facts',
 'WITH m AS (SELECT dim_a_id FROM tenant_scope GROUP BY 1 HAVING count(*) = 4 ORDER BY 1 LIMIT 1),
	d AS (DELETE FROM tenant_scope t USING m WHERE t.dim_a_id = m.dim_a_id AND t.tenant_id = (SELECT max(tenant_id) FROM tenant_scope WHERE dim_a_id = m.dim_a_id) RETURNING 1),
	f AS (DELETE FROM events e USING m WHERE e.dim_a_id = m.dim_a_id RETURNING 1)
  INSERT INTO tenant_scope SELECT m.dim_a_id, k FROM m, generate_series(1, 12) k WHERE NOT EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = m.dim_a_id AND s.tenant_id = k) ORDER BY k LIMIT 1',
 false, false, true),
(9, 'facts of 40 dim_a move to another entity (new versions: their keys are those of the new entity)',
 'UPDATE events SET dim_a_id = 1 + (dim_a_id * 7) % 300 WHERE dim_a_id % 8 = 2', false, false, false),
(10, 'new facts of dim_a of every number of keys, and of an unknown one',
 'INSERT INTO events SELECT 8000 + i, CASE WHEN i % 50 = 0 THEN 7777 ELSE 1 + i % 320 END, timestamptz ''2026-06-01'' + i * interval ''3 minutes'', i % 10, ''y'' FROM generate_series(1, 400) i', false, false, false),
(11, 'the pairs are replaced by the same pairs (truncate and load: a new lineage)',
 'CREATE TEMP TABLE all_pairs AS SELECT * FROM tenant_scope; TRUNCATE tenant_scope; INSERT INTO tenant_scope SELECT * FROM all_pairs', false, false, false),
(12, 'the scope summary is written again (REINDEX)', 'REINDEX INDEX events_col', false, true, false),
(13, 'a promotion of the new facts', 'SELECT sift.col_promote(''events_col'')', false, false, false),
(14, 'a merge', 'SELECT sift.col_merge(''events_col'', true)', false, false, false),
(15, 'a pair edit after the promotion and the merge: 25 dim_a move',
 'UPDATE tenant_scope t SET tenant_id = 1 + t.tenant_id % 12 WHERE dim_a_id % 12 = 5 AND NOT EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = t.dim_a_id AND s.tenant_id = 1 + t.tenant_id % 12)',
 false, false, true);

CREATE FUNCTION edit(step int, OUT same boolean, OUT scope_one_key boolean, OUT delta boolean) LANGUAGE plpgsql AS $$
DECLARE
	e edits;
	inside record;
	after record;
	inside_same boolean := true;
BEGIN
	SELECT * INTO e FROM edits WHERE n = step;
	IF e.abort THEN
		BEGIN
			EXECUTE e.script;
			SELECT * INTO inside FROM battery();
			inside_same := inside.same;
			RAISE EXCEPTION 'rollback';
		EXCEPTION WHEN raise_exception THEN NULL;
		END;
	ELSE
		EXECUTE e.script;
	END IF;
	SELECT * INTO after FROM battery();
	same := after.same AND inside_same;
	scope_one_key := CASE WHEN e.rebuild THEN after.scope_one_key END;
	-- an edit that changed entities leaves reads that take them as the changes since the summary
	delta := CASE WHEN e.patch THEN coalesce(after.delta, false) END;
END $$;

-- one transaction for each edit: the journal vouches for a read after the transaction that edited the pairs, not inside it
CREATE TABLE edit_results AS SELECT n, NULL::boolean AS same, NULL::boolean AS scope_one_key, NULL::boolean AS delta FROM edits;
CREATE FUNCTION run_edit(step int) RETURNS void LANGUAGE sql AS
$$ UPDATE edit_results SET (same, scope_one_key, delta) = (SELECT r.same, r.scope_one_key, r.delta FROM edit(step) r) WHERE n = step $$;
\o /dev/null
SELECT format('SELECT run_edit(%s)', n) FROM edits ORDER BY n \gexec
\o
SELECT e.n, e.descr, r.same, r.scope_one_key, r.delta FROM edits e JOIN edit_results r USING (n) ORDER BY e.n;
SELECT sift.col_verify('events_col', true);

-- (e) facts deleted or updated: the fact leaves every key of its entity (a read is the join at once), and so after VACUUM
DELETE FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope GROUP BY 1 HAVING count(*) >= 5);
SELECT b.same AS same_after_delete FROM battery() b;
VACUUM events;
SELECT b.same AS same_after_vacuum FROM battery() b;
UPDATE events SET v = v + 1, created_at = created_at + interval '1 hour' WHERE id % 7 = 0;
SELECT b.same AS same_after_update FROM battery() b;
VACUUM events;
SELECT b.same AS same_after_vacuum FROM battery() b;
SELECT sift.col_verify('events_col', true);

-- the pairs and the facts are what they were, written again: every read is the join, and every read of one key is a scope read
REINDEX INDEX events_col;
SELECT b.same AS same_after_rebuild, b.scope_one_key AS scope_after_rebuild FROM battery() b;

DROP TABLE battery_q, shapes, preds, edits, edit_results, events, tenant_scope;
DROP FUNCTION battery(), edit(int), run_edit(int);
DROP FUNCTION scope_run(text, text), scope_group_run(text), scope_reason_ok(text), scope_read(text);
DROP EXTENSION pg_sift;
