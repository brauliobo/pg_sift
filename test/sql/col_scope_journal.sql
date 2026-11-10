-- The journal of the scope relation (src/col/scope_journal.c, INTERFACE section 5, docs/COL-CONNECT.md "The rule"): a sift_col index of the relation with track_changes = 'ctid' and the
-- member key first keeps a KEYCHG record for the member of every pair an UPDATE, DELETE or key change ended and a KEYADD record for the member of every pair inserted. The scope segments
-- are read as stored only while the journal vouches for their stamp; the members that changed since are read through the entity rows (a top-N reads the entity rows of all the members), every
-- other state of the journal reads them all, with the same answer as the join. The verdicts are the journal's.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
-- members in up to six keys on purpose: above the default limit of the multiplicity
SET sift.scope_max_multiplicity = 8;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;

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

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT m, 1 + (m * 5 + j * 7) % 12 FROM generate_series(1, 300) m, generate_series(0, m % 3) j WHERE m <> 17 ON CONFLICT DO NOTHING;
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN 999 ELSE 1 + (i * 7919) % 320 END,
	timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 20000) i;
CREATE FUNCTION try(stmt text) RETURNS text LANGUAGE plpgsql AS
$$ DECLARE hint text; BEGIN EXECUTE stmt; RETURN 'ok';
   EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS hint = PG_EXCEPTION_HINT; RETURN sqlerrm || coalesce(' [' || hint || ']', ''); END $$;

-- a scope relation with no journal index: refused
SELECT try('CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = ''tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)'')') AS no_journal_index;
-- the journal index leads with the member, tracks by ctid and holds the scope key: these do not qualify
CREATE INDEX tenant_scope_wrong_order ON tenant_scope USING sift_col (tenant_id, dim_a_id) WITH (track_changes = 'ctid');
CREATE INDEX tenant_scope_no_key ON tenant_scope USING sift_col (dim_a_id) WITH (track_changes = 'ctid');
SELECT try('CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = ''tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)'')') AS not_qualifying;
DROP INDEX tenant_scope_wrong_order, tenant_scope_no_key;

-- the journal index, then the scope index: no journal exists until a scope index is declared on the relation
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
SELECT lineage = 0 AS no_journal_yet FROM sift._test_conn_gen('tenant_scope_journal');
INSERT INTO tenant_scope VALUES (400, 1);
DELETE FROM tenant_scope WHERE dim_a_id = 400;
SELECT count(*) AS records_before_a_scope FROM sift._test_col_log_entries('tenant_scope_journal') WHERE kind IN (4, 5);
SET client_min_messages = notice;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v)
	WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SET client_min_messages = warning;
SELECT (SELECT string_agg(refobjid::regclass::text, ' ') FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = 'events_col'::regclass AND refclassid = 'pg_class'::regclass
	AND deptype = 'n' AND (SELECT relkind FROM pg_class WHERE oid = refobjid) = 'i') AS edges;
SELECT try('DROP INDEX tenant_scope_journal') AS drop_journal;
SELECT lineage <> 0 AS journal_made FROM sift._test_conn_gen('tenant_scope_journal');
VACUUM (FREEZE, ANALYZE) events;
VACUUM (ANALYZE) tenant_scope;

CREATE TABLE shapes (n int, q text, plain text);
INSERT INTO shapes
SELECT n, replace(q, '@', 'sift.in_scope(''events_col''::regclass, ''tenant_id'', e.dim_a_id, ' || k || ')'),
	replace(plain, '@', 'e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ' || k || ')')
FROM (VALUES
	(1, 3, 'SELECT count(*), sum(v), min(id), max(id) FROM events e WHERE @'),
	(2, 4, 'SELECT v, count(*) FROM events e WHERE id > 5000 AND @ GROUP BY v ORDER BY v'),
	(3, 13, 'SELECT count(*), sum(v) FROM events e WHERE @'),
	(4, 3, 'SELECT id, created_at FROM events e WHERE @ ORDER BY created_at DESC, id DESC LIMIT 20')) s(n, k, q),
	LATERAL (SELECT q AS plain) p;
CREATE FUNCTION phase() RETURNS TABLE (n int, same boolean, stored boolean, why text) LANGUAGE sql AS $$
	SELECT s.n, r.same, r.stored, r.why FROM shapes s, LATERAL scope_run(s.q, s.plain) r ORDER BY s.n
$$;
-- one line for the four shapes: all answers equal the join, how many read the scope units, and the reasons of the others
CREATE FUNCTION verdict() RETURNS TABLE (all_same boolean, stored int, reasons text) LANGUAGE sql AS $$
	SELECT bool_and(same), count(*) FILTER (WHERE stored)::int, coalesce(string_agg(DISTINCT why, ', '), '-') FROM phase()
$$;

-- built under a journal that did not exist when the table was read: the segments carry no stamp (the next rewrite fills them), and the read says so
SELECT * FROM verdict();
SELECT bool_and(lineage = 0) AS unstamped FROM sift._test_col_conn_stamps('events_col');
-- a rebuild under the journal stamps them
REINDEX INDEX events_col;
SELECT bool_and(lineage = (SELECT lineage FROM sift._test_conn_gen('tenant_scope_journal'))) AS stamped_by_the_journal FROM sift._test_col_conn_stamps('events_col');
SELECT * FROM verdict();
SELECT why FROM sift._test_scope_take('events_col', 0);
SELECT lineage AS lin, horizon AS hor FROM sift._test_scope_take('events_col', 0) \gset
SELECT why, key FROM sift._test_scope_journal_changes('events_col', 0, :lin, :'hor');
SELECT sift._test_scope_vouch('events_col', 0, :lin, :'hor') AS vouch_now,
	sift._test_scope_vouch('events_col', 0, :lin + 1, :'hor') AS vouch_other_lineage,
	sift._test_scope_vouch('events_col', 0, :lin, ((:'hor')::text::bigint + 100000)::text::xid) AS vouch_snapshot_older;

-- every change of the scope relation: the members it changes are in the journal's set (the read leaves them out of the units and reads their entity rows; a top-N falls back), a rebuild stamps again
SELECT lineage AS lin, horizon AS hor FROM sift._test_scope_take('events_col', 0) \gset
-- (1) pairs inserted: KEYADD for each inserted pair (an existing pair under ON CONFLICT DO NOTHING is a speculative insertion that is killed: its record only widens the set)
INSERT INTO tenant_scope VALUES (1, 3), (2, 3), (301, 3), (302, 13) ON CONFLICT DO NOTHING;
SELECT kind, count(*) FROM sift._test_col_log_entries('tenant_scope_journal') WHERE kind IN (4, 5) GROUP BY kind ORDER BY kind;
SELECT why, key FROM sift._test_scope_journal_changes('events_col', 0, :lin, :'hor') ORDER BY key;
SELECT * FROM verdict();
VACUUM tenant_scope;
SELECT * FROM verdict();
REINDEX INDEX events_col;
SELECT * FROM verdict();

-- (2) pairs deleted: KEYCHG for the member of each
SELECT lineage AS lin, horizon AS hor FROM sift._test_scope_take('events_col', 0) \gset
DELETE FROM tenant_scope WHERE tenant_id = 3 AND dim_a_id % 5 = 0;
SELECT count(*) > 0 AS members_changed, min(key) AS first, bool_and(key % 5 = 0) AS all_multiples FROM sift._test_scope_journal_changes('events_col', 0, :lin, :'hor');
SELECT * FROM verdict();
REINDEX INDEX events_col;
SELECT * FROM verdict();

-- (3) pairs moved to another key, (4) given to another member, (5) upserted: the old member (KEYCHG) and the new one (KEYADD) both change
SELECT lineage AS lin, horizon AS hor FROM sift._test_scope_take('events_col', 0) \gset
UPDATE tenant_scope SET tenant_id = 13 WHERE tenant_id = 4 AND dim_a_id % 7 = 0;
SELECT count(*) > 0 AS members_changed FROM sift._test_scope_journal_changes('events_col', 0, :lin, :'hor') WHERE key IS NOT NULL;
SELECT * FROM verdict();
REINDEX INDEX events_col;
SELECT * FROM verdict();
SELECT lineage AS lin, horizon AS hor FROM sift._test_scope_take('events_col', 0) \gset
UPDATE tenant_scope SET dim_a_id = dim_a_id + 1000 WHERE tenant_id = 5 AND dim_a_id < 40;
SELECT count(*) FILTER (WHERE key < 1000) > 0 AS old_members, count(*) FILTER (WHERE key >= 1000) > 0 AS new_members FROM sift._test_scope_journal_changes('events_col', 0, :lin, :'hor');
SELECT * FROM verdict();
REINDEX INDEX events_col;
SELECT * FROM verdict();
INSERT INTO tenant_scope VALUES (3, 3), (5, 4) ON CONFLICT (dim_a_id, tenant_id) DO UPDATE SET tenant_id = 13;
SELECT * FROM verdict();
REINDEX INDEX events_col;
SELECT * FROM verdict();

-- rolled back changes leave nothing: a transaction, a savepoint, and the statement that fails
SELECT lineage AS lin, horizon AS hor FROM sift._test_scope_take('events_col', 0) \gset
BEGIN;
INSERT INTO tenant_scope VALUES (303, 3);
DELETE FROM tenant_scope WHERE dim_a_id = 20;
ROLLBACK;
BEGIN;
SAVEPOINT s;
DELETE FROM tenant_scope WHERE dim_a_id = 21;
INSERT INTO tenant_scope VALUES (304, 3);
ROLLBACK TO s;
COMMIT;
SELECT try('INSERT INTO tenant_scope VALUES (305, 3), (305, 3)') AS failed_insert;
SELECT count(*) AS changed_after_rollbacks FROM sift._test_scope_journal_changes('events_col', 0, :lin, :'hor') WHERE key IS NOT NULL;
SELECT * FROM verdict();
-- a savepoint that is released keeps its changes
BEGIN;
SAVEPOINT s;
DELETE FROM tenant_scope WHERE dim_a_id = 22;
RELEASE s;
COMMIT;
SELECT key FROM sift._test_scope_journal_changes('events_col', 0, :lin, :'hor') WHERE key IS NOT NULL;
REINDEX INDEX events_col;

-- the transaction's own change is in the set before it commits, and the answers are the join's under it
BEGIN;
DELETE FROM tenant_scope WHERE dim_a_id = 25;
SELECT * FROM verdict();
SELECT sift._test_scope_vouch('events_col', 0, :lin, :'hor') AS vouch_in_own_transaction;
ROLLBACK;
SELECT * FROM verdict();

-- snapshots that cannot be compared with the stamp
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT * FROM verdict();
ROLLBACK;

-- the fact rows change as ever: deleted, updated and inserted rows are the entity path's business (a write buffer is read through it), the journal says nothing of them
DELETE FROM events WHERE id % 13 = 0;
UPDATE events SET v = v + 1 WHERE id % 17 = 0;
SELECT * FROM verdict();
VACUUM events;
SELECT * FROM verdict();

-- TRUNCATE of the scope relation and REINDEX of its journal index make another journal: the stamp names the old one
CREATE TABLE tenant_scope_copy AS SELECT * FROM tenant_scope;
SELECT lineage AS lin_before FROM sift._test_conn_gen('tenant_scope_journal') \gset
REINDEX INDEX events_col;
SELECT * FROM verdict();
TRUNCATE tenant_scope;
INSERT INTO tenant_scope SELECT * FROM tenant_scope_copy;
SELECT lineage <> :lin_before AS new_lineage FROM sift._test_conn_gen('tenant_scope_journal');
SELECT * FROM verdict();
REINDEX INDEX events_col;
SELECT * FROM verdict();
SELECT lineage AS lin_before FROM sift._test_conn_gen('tenant_scope_journal') \gset
REINDEX INDEX tenant_scope_journal;
SELECT lineage <> :lin_before AS new_lineage, lineage <> 0 AS kept_journal FROM sift._test_conn_gen('tenant_scope_journal');
SELECT * FROM verdict();
REINDEX INDEX events_col;
SELECT * FROM verdict();
SELECT lineage AS lin_before FROM sift._test_conn_gen('tenant_scope_journal') \gset
REINDEX INDEX CONCURRENTLY tenant_scope_journal;
SELECT lineage <> :lin_before AS new_lineage FROM sift._test_conn_gen('tenant_scope_journal');
-- the log of a concurrently built copy is complete after the next VACUUM: nothing is vouched before
SELECT * FROM verdict();
VACUUM tenant_scope;
SELECT * FROM verdict();
REINDEX INDEX events_col;
SELECT * FROM verdict();
SELECT edge FROM (SELECT refobjid::regclass::text AS edge FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = 'events_col'::regclass AND refclassid = 'pg_class'::regclass
	AND deptype = 'n' AND (SELECT relkind FROM pg_class WHERE oid = refobjid) = 'i') e ORDER BY 1;

-- refresh: a few changed members are left to the reads, as many as sift.col_connect_refresh_pct percent of the pairs make the scope segments due for the rewrite ('stale connection')
REINDEX INDEX events_col;
SELECT sift.col_maintenance_due('events_col', false) AS due_filled;
DELETE FROM tenant_scope WHERE dim_a_id <= 5;
SELECT sift.col_maintenance_due('events_col', false) AS due_few_changed;
DELETE FROM tenant_scope WHERE dim_a_id <= 120;
SELECT sift.col_maintenance_due('events_col', false) AS due_many_changed;
REINDEX INDEX events_col;
SELECT sift.col_maintenance_due('events_col', false) AS due_refilled;
SET sift.col_connect_refresh_pct = 1;
INSERT INTO tenant_scope SELECT i, 1 FROM generate_series(1, 20) i;
SELECT sift.col_maintenance_due('events_col', false) AS due_by_the_setting;
RESET sift.col_connect_refresh_pct;
REINDEX INDEX events_col;

-- the journal and its records survive a VACUUM of the relation; a rewrite refills under a new stamp, so the stale rule ends
SELECT lineage AS lin, horizon AS hor FROM sift._test_scope_take('events_col', 0) \gset
DELETE FROM tenant_scope WHERE dim_a_id = 23;
VACUUM tenant_scope;
SELECT key FROM sift._test_scope_journal_changes('events_col', 0, :lin, :'hor') WHERE key IS NOT NULL;
SELECT why FROM sift._test_scope_journal_changes('events_col', 0, :lin, :'hor', 0) LIMIT 1;

-- the multiplicity of a scope key is said when the index is built and bounded by sift.scope_max_multiplicity
SET client_min_messages = notice;
REINDEX INDEX events_col;
SET client_min_messages = warning;
SET sift.scope_max_multiplicity = 1.0;
SELECT try('REINDEX INDEX events_col') AS refused;
SET sift.scope_max_multiplicity = 8;
SELECT * FROM verdict();

SELECT sift.col_verify('events_col', true);
DROP TABLE shapes, events, tenant_scope, tenant_scope_copy;
DROP FUNCTION phase(), verdict(), scope_run(text, text), try(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
