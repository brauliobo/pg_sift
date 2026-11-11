-- The journals of a scope of two paths (src/col/scope_journal.c): the grants keyed by member, the grants keyed by group and the memberships keyed by member, each a sift_col index with
-- track_changes = 'ctid' that starts when the scope index is built or rebuilt. A scope with one of them missing vouches for nothing ("no journal") and its reads are the entity rows; the journal
-- made later is found by the next REINDEX. A journal rebuilt (another lineage) leaves the stamps of the others standing and the scope unvouched until its own REINDEX. Refresh counts the members a
-- changed group has as it counts the changed members. An index whose scopes all have one path keeps column format 45 and one stamp for each scope, one with two paths writes 49 and three, and a
-- segment whose stamps are not those of the scope as declared now is not read as stored.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;

CREATE TABLE grants (id int PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX grants_direct ON grants (dim_a_id, tenant_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX grants_group ON grants (group_id, tenant_id) WHERE deleted_at IS NULL;
CREATE TABLE memberships (id serial PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX memberships_active ON memberships (group_id, dim_a_id) WHERE deleted_at IS NULL;
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int) WITH (autovacuum_enabled = false);
-- groups 1..6 of ten members each; tenant 1 has a group and a member of another, tenant 2 two groups
INSERT INTO memberships (group_id, dim_a_id) SELECT 1 + (m - 1) / 10, m FROM generate_series(1, 60) m;
INSERT INTO grants VALUES (1, 1, NULL, 1, NULL), (2, 1, 25, NULL, NULL), (3, 2, NULL, 2, NULL), (4, 2, NULL, 3, NULL), (5, 3, 41, NULL, NULL);
INSERT INTO facts SELECT i, 1 + (i * 7) % 60, timestamptz '2026-01-01' + i * interval '9 minutes', i % 10 FROM generate_series(1, 3000) i;
CREATE VIEW pair_oracle AS
	SELECT dim_a_id AS member, tenant_id FROM grants WHERE deleted_at IS NULL AND dim_a_id IS NOT NULL
	UNION
	SELECT m.dim_a_id, g.tenant_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL;

-- the read of the tenants 1 and 2 together and as the join says it; its plan says whether the units were read and why not
CREATE FUNCTION probe(OUT same boolean, OUT units boolean, OUT why text) LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	a text;
	b text;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT count(*), sum(amount) FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''tenant_id'', f.dim_a_id, ARRAY[1, 2]::int8[])' INTO p;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (SELECT count(*), sum(amount) FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''tenant_id'', f.dim_a_id, ARRAY[1, 2]::int8[])) t' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (SELECT count(*), sum(amount) FROM facts f WHERE f.dim_a_id IN (SELECT member FROM pair_oracle WHERE tenant_id IN (1, 2))) t' INTO b;
	SET LOCAL sift.enable_agg = on;
	same := a = b;
	units := jsonb_path_exists(p, '$.**."Scope Units Read"');
	why := jsonb_path_query_first(p, '$.**."Scope Fallback"') #>> '{}';
END $$;
-- the stamps of the scope as the journals give them now: how many journals vouch, and the reason when one does not
CREATE FUNCTION journals(OUT vouching int, OUT why text) LANGUAGE plpgsql AS $$
BEGIN
	SELECT count(*) FILTER (WHERE lineage <> 0)::int, min(s.why) INTO vouching, why FROM sift._test_scope_stamps('facts_col', 0) s;
END $$;
-- why the scope of an index is not read from its units (NULL: it is)
CREATE FUNCTION fallback(idx regclass) RETURNS text LANGUAGE plpgsql AS $$
DECLARE p jsonb;
BEGIN
	EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT count(*) FROM facts f WHERE sift.in_scope(%L::regclass, ''tenant_id'', f.dim_a_id, 1)', idx) INTO p;
	RETURN jsonb_path_query_first(p, '$.**."Scope Fallback"') #>> '{}';
END $$;

CREATE TABLE journal_ddl (n int, name text, ddl text);
INSERT INTO journal_ddl VALUES
	(1, 'grants_by_member', 'CREATE INDEX grants_by_member ON grants USING sift_col (dim_a_id, tenant_id) WITH (track_changes = ''ctid'')'),
	(2, 'grants_by_group', 'CREATE INDEX grants_by_group ON grants USING sift_col (group_id, tenant_id) WITH (track_changes = ''ctid'')'),
	(3, 'memberships_by_member', 'CREATE INDEX memberships_by_member ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = ''ctid'')');
CREATE TABLE scope_ddl AS SELECT 'CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = ''tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)'')' AS ddl;

-- no journal at all, then each of the three missing in turn: the scope is built, vouches for nothing and says so; the journal made, a REINDEX of the scope index starts it (the table was read before),
-- and the next one stamps the segments, which are read as stored
SELECT ddl AS scope_index FROM scope_ddl \gset
:scope_index;
SELECT 'none' AS missing, 'built' AS step, * FROM journals() j, probe() p;
SELECT stmt FROM (
	SELECT m.n, 0 AS k, 'DROP INDEX IF EXISTS facts_col' AS stmt FROM generate_series(1, 3) m(n)
	UNION ALL SELECT m.n, 1, 'DROP INDEX IF EXISTS grants_by_member, grants_by_group, memberships_by_member' FROM generate_series(1, 3) m(n)
	UNION ALL SELECT m.n, 2, j.ddl FROM generate_series(1, 3) m(n), journal_ddl j WHERE j.n <> m.n
	UNION ALL SELECT m.n, 3, ddl FROM generate_series(1, 3) m(n), scope_ddl
	UNION ALL SELECT m.n, 4, 'REINDEX INDEX facts_col' FROM generate_series(1, 3) m(n)
	UNION ALL SELECT m.n, 5, format('SELECT %L AS missing, ''built'' AS step, * FROM journals() j, probe() p', (SELECT name FROM journal_ddl WHERE n = m.n)) FROM generate_series(1, 3) m(n)
	UNION ALL SELECT m.n, 6, j.ddl FROM generate_series(1, 3) m(n), journal_ddl j WHERE j.n = m.n
	UNION ALL SELECT m.n, 7, 'REINDEX INDEX facts_col' FROM generate_series(1, 3) m(n)
	UNION ALL SELECT m.n, 8, format('SELECT %L AS missing, ''journal made, REINDEX'' AS step, * FROM journals() j, probe() p', (SELECT name FROM journal_ddl WHERE n = m.n)) FROM generate_series(1, 3) m(n)
	UNION ALL SELECT m.n, 9, 'REINDEX INDEX facts_col' FROM generate_series(1, 3) m(n)
	UNION ALL SELECT m.n, 10, format('SELECT %L AS missing, ''REINDEX again'' AS step, * FROM journals() j, probe() p', (SELECT name FROM journal_ddl WHERE n = m.n)) FROM generate_series(1, 3) m(n)
	) t ORDER BY n, k \gexec

-- the journal indexes the scope index found are its dependencies: they cannot be dropped under it
SELECT string_agg(refobjid::regclass::text, ' ' ORDER BY refobjid::regclass::text) AS edges FROM pg_depend
	WHERE classid = 'pg_class'::regclass AND objid = 'facts_col'::regclass AND refclassid = 'pg_class'::regclass AND deptype = 'n' AND (SELECT relkind FROM pg_class WHERE oid = refobjid) = 'i';
DO $$ BEGIN EXECUTE 'DROP INDEX grants_by_group'; EXCEPTION WHEN dependent_objects_still_exist THEN RAISE WARNING 'refused: %', sqlerrm; END $$;

-- a journal rebuilt is another journal: the stamps of the segments name the old one, the others stand, and the scope is unvouched until its own REINDEX
REINDEX INDEX facts_col;
SELECT 'built' AS journal_rebuilt, * FROM journals() j, probe() p;
SELECT stmt FROM (
	SELECT j.n, 0 AS k, format('REINDEX INDEX %s', j.name) AS stmt FROM journal_ddl j
	UNION ALL SELECT j.n, 1, format('SELECT %L AS journal_rebuilt, * FROM journals() j, probe() p', j.name) FROM journal_ddl j
	UNION ALL SELECT j.n, 2, 'REINDEX INDEX facts_col' FROM journal_ddl j
	UNION ALL SELECT j.n, 3, format('SELECT %L AS journal_rebuilt, ''scope index rebuilt'' AS step, * FROM journals() j, probe() p', j.name) FROM journal_ddl j
	) t ORDER BY n, k \gexec

-- the stamps of a segment, each journal's own: a stamp of one journal that is not the journal's now is another lineage, whichever it is
SELECT lineage AS l0, horizon AS hor FROM sift._test_scope_stamps('facts_col', 0) WHERE journal = 0 \gset
SELECT lineage AS l1 FROM sift._test_scope_stamps('facts_col', 0) WHERE journal = 1 \gset
SELECT lineage AS l2 FROM sift._test_scope_stamps('facts_col', 0) WHERE journal = 2 \gset
SELECT sift._test_scope_vouch('facts_col', 0, :l0, :'hor', :l1, :l2) AS all_three,
	sift._test_scope_vouch('facts_col', 0, :l0 + 1, :'hor', :l1, :l2) AS first_other,
	sift._test_scope_vouch('facts_col', 0, :l0, :'hor', :l1 + 1, :l2) AS second_other,
	sift._test_scope_vouch('facts_col', 0, :l0, :'hor', :l1, :l2 + 1) AS third_other,
	sift._test_scope_vouch('facts_col', 0, :l0, ((:'hor')::text::bigint + 100000)::text::xid, :l1, :l2) AS snapshot_older;

-- the members that changed: those of the first and the third journal, and those the memberships list for each group of the second (under the snapshot of the reader), at most the cap
SELECT key AS member FROM sift._test_scope_journal_changes('facts_col', 0, :l0, :'hor', 1000, :l1, :l2) WHERE key IS NOT NULL;
UPDATE grants SET deleted_at = now() WHERE id = 3;
SELECT why, count(key) AS members, min(key), max(key) FROM sift._test_scope_journal_changes('facts_col', 0, :l0, :'hor', 1000, :l1, :l2) GROUP BY why;
UPDATE grants SET deleted_at = NULL WHERE id = 3;
UPDATE memberships SET deleted_at = now() WHERE group_id = 1 AND dim_a_id = 3;
SELECT why, count(key) AS members, min(key), max(key) FROM sift._test_scope_journal_changes('facts_col', 0, :l0, :'hor', 1000, :l1, :l2) GROUP BY why;
UPDATE memberships SET deleted_at = NULL WHERE group_id = 1 AND dim_a_id = 3;
UPDATE grants SET deleted_at = now() WHERE id = 5;
SELECT why, count(key) AS members, min(key), max(key) FROM sift._test_scope_journal_changes('facts_col', 0, :l0, :'hor', 1000, :l1, :l2) GROUP BY why;
SELECT why, key FROM sift._test_scope_journal_changes('facts_col', 0, :l0, :'hor', 0, :l1, :l2);
-- a group that has more members than the cap is over it
UPDATE grants SET deleted_at = now() WHERE id = 3;
SELECT why, key FROM sift._test_scope_journal_changes('facts_col', 0, :l0, :'hor', 5, :l1, :l2);
SET sift.scope_patch_keys = 5;
SELECT * FROM probe();
RESET sift.scope_patch_keys;
SELECT * FROM probe();
UPDATE grants SET deleted_at = NULL WHERE id IN (3, 5);

-- refresh: the members of a changed group count as changed members (sift.col_connect_refresh_pct percent, 10, of the rows of the pairs relation and the via relation, 65 here: 6.5 members); a few are left to the reads
VACUUM (ANALYZE) grants, memberships;
REINDEX INDEX facts_col;
SELECT sift.col_maintenance_due('facts_col', false) AS due_filled;
UPDATE grants SET deleted_at = now() WHERE id = 5;
SELECT sift.col_maintenance_due('facts_col', false) AS due_a_member;
UPDATE grants SET deleted_at = NULL WHERE id = 5;
UPDATE memberships SET deleted_at = now() WHERE group_id = 2 AND dim_a_id <= 18;
SELECT sift.col_maintenance_due('facts_col', false) AS due_members_changed;
UPDATE memberships SET deleted_at = NULL WHERE group_id = 2 AND dim_a_id <= 18;
REINDEX INDEX facts_col;
SELECT sift.col_maintenance_due('facts_col', false) AS due_filled_again;
UPDATE grants SET deleted_at = now() WHERE id = 3;
SELECT sift.col_maintenance_due('facts_col', false) AS due_a_group;
UPDATE grants SET deleted_at = NULL WHERE id = 3;
REINDEX INDEX facts_col;
SELECT * FROM probe();
-- a journal rebuilt after the build makes the segments stale: they are refilled
REINDEX INDEX grants_by_group;
SELECT sift.col_maintenance_due('facts_col', false) AS due_other_lineage;
REINDEX INDEX facts_col;
SELECT sift.col_maintenance_due('facts_col', false) AS due_refilled;

-- column format: a scope of two paths is 49 with a stamp for each journal, one of one path stays 45 with one, and an older stamp layout of a scope that is declared otherwise now is not read as stored
CREATE TABLE pairs_one (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO pairs_one SELECT m, 1 + m % 3 FROM generate_series(1, 60) m;
CREATE INDEX pairs_one_journal ON pairs_one USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX facts_one ON facts USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = 'tenant_id: dim_a_id -> pairs_one(dim_a_id, tenant_id)');
REINDEX INDEX facts_one;
SELECT 'two paths' AS scope, format, count(*) AS segments FROM sift.col_segments('facts_col') WHERE kind = 'immutable' GROUP BY format
	UNION ALL SELECT 'one path', format, count(*) FROM sift.col_segments('facts_one') WHERE kind = 'immutable' GROUP BY format ORDER BY 1;
SELECT 'two paths' AS scope, journal, count(*) AS stamps, count(*) FILTER (WHERE lineage <> 0) AS filled FROM sift._test_col_scope_journal_stamps('facts_col') GROUP BY journal
	UNION ALL SELECT 'one path', journal, count(*), count(*) FILTER (WHERE lineage <> 0) FROM sift._test_col_scope_journal_stamps('facts_one') GROUP BY journal ORDER BY 1, 2;
SELECT sift.col_verify('facts_one', true);
SELECT sift.col_verify('facts_col', true);
-- the same relation declared with a second path (ALTER INDEX): the segments hold one stamp and the scope has three now
ALTER INDEX facts_one SET (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)');
SELECT journal, count(*) AS stamps FROM sift._test_col_scope_journal_stamps('facts_one') GROUP BY journal ORDER BY 1;
SELECT sift.col_maintenance_due('facts_one', false) AS due_declared_otherwise;
SELECT fallback('facts_one') AS read_says;
REINDEX INDEX facts_one;
SELECT format, count(*) AS segments FROM sift.col_segments('facts_one') WHERE kind = 'immutable' GROUP BY format;
SELECT journal, count(*) AS stamps FROM sift._test_col_scope_journal_stamps('facts_one') GROUP BY journal ORDER BY 1;
-- and back to one path: three stamps of a scope that has one are not its stamp
ALTER INDEX facts_one SET (scope = 'tenant_id: dim_a_id -> pairs_one(dim_a_id, tenant_id)');
SELECT sift.col_maintenance_due('facts_one', false) AS due_declared_otherwise;
SELECT fallback('facts_one') AS read_says;
REINDEX INDEX facts_one;
SELECT format, count(*) AS segments FROM sift.col_segments('facts_one') WHERE kind = 'immutable' GROUP BY format;
SELECT sift.col_verify('facts_one', true);

DROP TABLE facts, grants, memberships, pairs_one, journal_ddl, scope_ddl CASCADE;
DROP FUNCTION probe(), journals(), fallback(regclass);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
