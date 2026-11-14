-- The journal index of a relation of pairs is made by CREATE INDEX of the scope index when the relation has none (src/col/scope_make.c, docs/COL-SCOPE.md "The journal index"): named by the rule, owned by
-- the owner of the table, marked by its comment, with the dependency of the scope index on it, and dropped with the last scope index that uses it. A journal made by hand, or whose comment was
-- changed, is the user's and stays. A role that does not own the pairs cannot make it.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET sift.scope_max_multiplicity = 8;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT m, 1 + (m * 5 + j * 7) % 12 FROM generate_series(1, 300) m, generate_series(0, m % 3) j WHERE m <> 17 ON CONFLICT DO NOTHING;
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + (i * 7919) % 320, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 20000) i;
CREATE FUNCTION try(stmt text) RETURNS text LANGUAGE plpgsql AS
$$ DECLARE hint text; BEGIN EXECUTE stmt; RETURN 'ok';
   EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS hint = PG_EXCEPTION_HINT; RETURN sqlerrm || coalesce(' [' || hint || ']', ''); END $$;
-- the indexes of the pairs relation with what makes a journal of one: its name, owner, options, whether it has a comment, and the scope indexes that depend on it
CREATE VIEW journals AS
SELECT c.relname AS journal, pg_get_userbyid(c.relowner) AS owner, c.reloptions AS options, obj_description(c.oid, 'pg_class') IS NOT NULL AS commented,
	   (SELECT string_agg(d.objid::regclass::text, ' ' ORDER BY d.objid::regclass::text) FROM pg_depend d
		 WHERE d.refobjid = c.oid AND d.classid = 'pg_class'::regclass AND d.deptype = 'n') AS used_by
FROM pg_class c JOIN pg_index i ON i.indexrelid = c.oid
WHERE i.indrelid = 'tenant_scope'::regclass AND c.relam = (SELECT oid FROM pg_am WHERE amname = 'sift_col') ORDER BY 1;
CREATE FUNCTION unfilled(ix regclass) RETURNS boolean LANGUAGE sql AS $$ SELECT sift.col_maintenance_due(ix, false) IS NOT NULL $$;
CREATE FUNCTION same_as_join() RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	SELECT md5(json_agg(t)::text) INTO a FROM (SELECT k, count(*), sum(v) FROM generate_series(1, 12) k, LATERAL
		(SELECT e.v FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = k)) x GROUP BY k ORDER BY k) t;
	SET LOCAL sift.enable_agg = off;
	SELECT md5(json_agg(t)::text) INTO b FROM (SELECT k, count(*), sum(v) FROM generate_series(1, 12) k, LATERAL
		(SELECT e.v FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = k)) x GROUP BY k ORDER BY k) t;
	RETURN a = b;
END $$;

-- a relation of pairs with no journal index: the statement makes it, says so, and the segments are stamped by it (the reads are the scope's from the start)
SET client_min_messages = notice;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SET client_min_messages = warning;
SELECT * FROM journals;
SELECT unfilled('events_col') AS unfilled, same_as_join() AS same;
SELECT count(*) AS stored_reads FROM sift._test_col_scope_stamps('events_col') WHERE family = 'scope' AND lineage <> 0;

-- a second scope index of the relation finds the journal and makes none
SET client_min_messages = notice;
CREATE INDEX events_col2 ON events USING sift_col (id, dim_a_id, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SET client_min_messages = warning;
SELECT * FROM journals;

-- the journal goes with the last scope index that uses it
DROP INDEX events_col;
SELECT journal, used_by FROM journals;
DROP INDEX events_col2;
SELECT journal, used_by FROM journals;

-- a journal made by hand is found, named as its user chose, and stays when the scope index is dropped; one that does not qualify is left alone and a journal is made
CREATE INDEX tenant_scope_wrong_order ON tenant_scope USING sift_col (tenant_id, dim_a_id) WITH (track_changes = 'ctid');
CREATE INDEX tenant_scope_by_hand ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT journal, commented, used_by FROM journals;
DROP INDEX events_col;
SELECT journal, used_by FROM journals;
DROP INDEX tenant_scope_by_hand;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT journal, commented, used_by FROM journals;
DROP INDEX tenant_scope_wrong_order;

-- a comment of the user's on the journal makes it theirs: it stays
COMMENT ON INDEX tenant_scope_dim_a_id_tenant_id_sift_journal IS 'the grants journal';
DROP INDEX events_col;
SELECT journal, commented, used_by FROM journals;
DROP INDEX tenant_scope_dim_a_id_tenant_id_sift_journal;

-- ALTER INDEX .. RESET (scope) (an index that has segments refuses it) and DROP TABLE drop the journal too, the other ways a scope index goes
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT count(*) AS journals FROM journals;
ALTER INDEX events_col RESET (scope);
SELECT count(*) AS journals_after_refused_reset FROM journals;
TRUNCATE events;
ALTER INDEX events_col RESET (scope);
SELECT count(*) AS journals_after_reset FROM journals;
ALTER INDEX events_col SET (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT count(*) AS journals_after_set FROM journals;
DROP TABLE events;
SELECT count(*) AS journals_after_drop_table FROM journals;

-- a concurrent build makes it too (as the plain statement: the writers of the pairs wait for the journal, not for the build of the facts); its segments carry no stamp until the maintenance or a REINDEX fills them
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + (i * 7919) % 320, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 20000) i;
CREATE INDEX CONCURRENTLY events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT journal, commented, used_by FROM journals;
SELECT same_as_join() AS same;
DROP INDEX events_col;
SELECT count(*) AS journals_after_drop FROM journals;

-- a role that owns the facts but not the pairs cannot make the journal, and says who can; the owner of both makes it, and it is theirs
CREATE ROLE journal_facts_owner;
CREATE ROLE journal_pairs_owner;
GRANT CREATE ON SCHEMA public TO journal_facts_owner, journal_pairs_owner;
GRANT SELECT ON tenant_scope TO journal_facts_owner;	-- the role that builds a scope index reads its pairs (col_scope_privileges)
ALTER TABLE events OWNER TO journal_facts_owner;
SET ROLE journal_facts_owner;
SELECT try('CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = ''tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)'')') AS not_the_owner;
RESET ROLE;
ALTER TABLE tenant_scope OWNER TO journal_pairs_owner;
SET ROLE journal_pairs_owner;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
RESET ROLE;
SET ROLE journal_facts_owner;
SELECT try('CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = ''tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)'')') AS journal_made_by_the_owner;
RESET ROLE;
ALTER TABLE tenant_scope OWNER TO CURRENT_USER;
DROP INDEX events_col;
DROP INDEX tenant_scope_journal;
ALTER TABLE events OWNER TO CURRENT_USER;
DROP OWNED BY journal_facts_owner, journal_pairs_owner;
DROP ROLE journal_facts_owner, journal_pairs_owner;

-- a scope of two paths makes the journals of its three relations
CREATE TABLE grants (id serial PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int, deleted_at timestamptz, CHECK (dim_a_id IS NULL OR group_id IS NULL)) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX grants_direct ON grants (dim_a_id, tenant_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX grants_group ON grants (group_id, tenant_id) WHERE deleted_at IS NULL;
CREATE TABLE memberships (id serial PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX memberships_active ON memberships (group_id, dim_a_id) WHERE deleted_at IS NULL;
INSERT INTO memberships (group_id, dim_a_id) SELECT 1 + m % 12, m FROM generate_series(1, 200) m;
INSERT INTO grants (tenant_id, dim_a_id) SELECT 1 + (m * 31) % 16, m FROM generate_series(1, 200) m ON CONFLICT DO NOTHING;
INSERT INTO grants (tenant_id, group_id) SELECT 1 + (g * 5) % 16, g FROM generate_series(1, 12) g ON CONFLICT DO NOTHING;
SET client_min_messages = notice;
CREATE INDEX hop_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)');
SET client_min_messages = warning;
SELECT c.relname AS journal, (SELECT count(*) FROM pg_depend d WHERE d.refobjid = c.oid AND d.objid = 'hop_col'::regclass AND d.deptype = 'n') AS used_by_hop_col
FROM pg_class c JOIN pg_index i ON i.indexrelid = c.oid WHERE c.relam = (SELECT oid FROM pg_am WHERE amname = 'sift_col') AND i.indrelid IN ('grants'::regclass, 'memberships'::regclass) ORDER BY 1;
DROP INDEX hop_col;
SELECT count(*) AS journals_after_drop FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE c.relam = (SELECT oid FROM pg_am WHERE amname = 'sift_col');

DROP TABLE events, tenant_scope, grants, memberships CASCADE;
DROP FUNCTION try(text), unfilled(regclass), same_as_join();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
