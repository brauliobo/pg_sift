-- What a scope declaration needs of its relations and its journals is said when the index is made or altered, not when it is first read: a temporary table of pairs, a relation with
-- row-level security, a scope of two paths without its journals (refused), a partition, an ALTER INDEX that declares or gives up a scope the segments do not hold (refused), a journal
-- that is not one, and an ALTER INDEX that would take the journal away.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, kind int NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + i % 50, i % 4, i % 10 FROM generate_series(1, 2000) i;
CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT i, 1 + i % 5 FROM generate_series(1, 50) i;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE FUNCTION try(stmt text) RETURNS text LANGUAGE plpgsql AS
$$ DECLARE hint text; detail text; BEGIN EXECUTE stmt; RETURN 'ok';
   EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS hint = PG_EXCEPTION_HINT, detail = PG_EXCEPTION_DETAIL;
   RETURN sqlerrm || coalesce(' {' || nullif(detail, '') || '}', '') || coalesce(' [' || hint || ']', ''); END $$;

-- a temporary table of pairs: the index would be dropped with it when the session ends
CREATE TEMP TABLE pairs_temp (m int NOT NULL, s int NOT NULL, PRIMARY KEY (m, s));
SELECT try('CREATE INDEX temp_col ON events USING sift_col (id, dim_a_id, amount) WITH (scope = ''tenant_id: dim_a_id -> pg_temp.pairs_temp(m, s)'')') AS temp_pairs;

-- a sift_col index of the relation that is not its journal (track_changes off, or the member second) is left alone: a journal is made beside it
CREATE TABLE pairs_unjournaled (m int NOT NULL, s int NOT NULL, PRIMARY KEY (m, s));
CREATE INDEX pairs_unjournaled_auto ON pairs_unjournaled USING sift_col (m, s) WITH (track_changes = 'off');
CREATE INDEX unjournaled_col ON events USING sift_col (id, dim_a_id, amount) WITH (scope = 'tenant_id: dim_a_id -> pairs_unjournaled(m, s)');
SELECT string_agg(refobjid::regclass::text, ' ' ORDER BY refobjid::regclass::text) AS journal_beside_track_off FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = 'unjournaled_col'::regclass AND refclassid = 'pg_class'::regclass AND deptype = 'n' AND (SELECT relkind FROM pg_class WHERE oid = refobjid) = 'i';
DROP INDEX unjournaled_col;
DROP INDEX pairs_unjournaled_auto;
CREATE INDEX pairs_unjournaled_rev ON pairs_unjournaled USING sift_col (s, m) WITH (track_changes = 'ctid');
CREATE INDEX unjournaled_col ON events USING sift_col (id, dim_a_id, amount) WITH (scope = 'tenant_id: dim_a_id -> pairs_unjournaled(m, s)');
SELECT string_agg(refobjid::regclass::text, ' ' ORDER BY refobjid::regclass::text) AS journal_beside_member_second FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = 'unjournaled_col'::regclass AND refclassid = 'pg_class'::regclass AND deptype = 'n' AND (SELECT relkind FROM pg_class WHERE oid = refobjid) = 'i';
DROP INDEX unjournaled_col;

-- row-level security of the relation: the statements of a role the policies apply to read the join as written
CREATE TABLE pairs_rls (m int NOT NULL, s int NOT NULL, PRIMARY KEY (m, s));
ALTER TABLE pairs_rls ENABLE ROW LEVEL SECURITY;
CREATE INDEX pairs_rls_journal ON pairs_rls USING sift_col (m, s) WITH (track_changes = 'ctid');
CREATE INDEX rls_col ON events USING sift_col (id, dim_a_id, amount) WITH (scope = 'tenant_id: dim_a_id -> pairs_rls(m, s)');
DROP INDEX rls_col;

-- a scope of two paths needs the journals of its three relations, as a scope of one path needs its: the build makes them
CREATE TABLE grants (dim_a_id int, group_id int, tenant_id int NOT NULL);
CREATE UNIQUE INDEX grants_direct ON grants (dim_a_id, tenant_id);
CREATE UNIQUE INDEX grants_group ON grants (group_id, tenant_id);
CREATE TABLE memberships (group_id int NOT NULL, dim_a_id int NOT NULL, PRIMARY KEY (group_id, dim_a_id));
CREATE INDEX two_col ON events USING sift_col (id, dim_a_id, amount) WITH (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)');
SELECT string_agg(refobjid::regclass::text, ' ' ORDER BY refobjid::regclass::text) AS two_paths_journals FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = 'two_col'::regclass AND refclassid = 'pg_class'::regclass AND deptype = 'n' AND (SELECT relkind FROM pg_class WHERE oid = refobjid) = 'i';
DROP INDEX two_col;

-- a partition declares its own scope: a statement over the partitioned table reads none
CREATE TABLE part_events (id int, dim_a_id int NOT NULL, kind int NOT NULL, amount int NOT NULL) PARTITION BY RANGE (id);
CREATE TABLE part_events_1 PARTITION OF part_events FOR VALUES FROM (0) TO (100000);
CREATE INDEX part_col ON part_events USING sift_col (id, dim_a_id, amount) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
DROP TABLE part_events;

-- ALTER INDEX .. RESET (scope) would leave segments that hold a scope the index no longer declares: it is refused, and the statements over the table run on the scope
CREATE INDEX altered_col ON events USING sift_col (id, dim_a_id, amount) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
ALTER INDEX altered_col RESET (scope);
SELECT count(*) AS by_key FROM events WHERE id = 5;
SELECT sum(amount) AS total FROM events;
SELECT sum(e.amount) AS tenant_total FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 3);
DROP INDEX altered_col;

-- the journal of a scope keeps its log; one nothing depends on may be switched off and on
ALTER INDEX tenant_scope_journal SET (track_changes = 'off');
ALTER INDEX tenant_scope_journal SET (track_changes = 'ctid');
CREATE INDEX altered_col ON events USING sift_col (id, dim_a_id, amount) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT try('ALTER INDEX tenant_scope_journal SET (track_changes = ''off'')') AS journal_off_of_a_scope;
SELECT try('ALTER INDEX tenant_scope_journal RESET (track_changes)') AS journal_reset_of_a_scope;
SELECT try('ALTER INDEX tenant_scope_journal SET (track_changes = ''ctid'')') AS journal_kept;
DROP INDEX altered_col;

DROP TABLE events, tenant_scope, pairs_unjournaled, pairs_rls, grants, memberships CASCADE;
DROP FUNCTION try(text);
DROP EXTENSION pg_sift;
