-- A scope of two paths (the grants a tenant has on a member, and on the groups of the member) keeps the units of its distinct pairs and the stamps of the three journals its pairs depend on:
-- the grants keyed by member (J1), the grants keyed by group (J2) and the memberships keyed by member (J3). What changed since the stamps is the members of J1 and J3 and the members the
-- memberships list for each group of J2; a read takes the units less those members and reads their entity rows. Every state below is built from fresh stamps (REINDEX), edited once in the way
-- that exercises one of the dependencies, and read in the shapes a scope has (IN, EXISTS, GROUP BY, a join counting each pair, one key and several, top-N): the answers are the join's, and a read
-- that does not take the units says why (scope_reason_ok).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;
\ir test/scope_helpers.sql

CREATE TABLE grants (id int PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int, deleted_at timestamptz, note text) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX grants_direct ON grants (dim_a_id, tenant_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX grants_group ON grants (group_id, tenant_id) WHERE deleted_at IS NULL;
CREATE TABLE memberships (id serial PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz, note text) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX memberships_active ON memberships (group_id, dim_a_id) WHERE deleted_at IS NULL;
CREATE INDEX grants_by_member ON grants USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX grants_by_group ON grants USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX memberships_by_member ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int) WITH (autovacuum_enabled = false);

-- members 1..58 are in the group 1 + m % 6, every fifth in a second group, every eleventh of them soft deleted from the first; members 59 and 60 are in none
INSERT INTO memberships (group_id, dim_a_id, deleted_at) SELECT 1 + m % 6, m, CASE WHEN m % 11 = 0 THEN now() END FROM generate_series(1, 58) m;
INSERT INTO memberships (group_id, dim_a_id) SELECT 1 + (m + 2) % 6, m FROM generate_series(5, 58, 5) m;
INSERT INTO grants VALUES
	(1, 1, 5, NULL, NULL),		-- tenant 1: member 5 directly, and through its two groups (6 and 2)
	(2, 1, 17, NULL, NULL),
	(3, 1, NULL, 6, NULL),
	(4, 1, NULL, 2, NULL),
	(5, 2, NULL, 3, NULL),		-- tenant 2: a group, and a soft deleted direct grant
	(6, 2, 8, NULL, now()),
	(7, 3, 1, NULL, NULL),		-- tenant 3: three members, and a soft deleted group
	(8, 3, 2, NULL, NULL),
	(9, 3, 3, NULL, NULL),
	(10, 3, NULL, 4, now()),
	(11, 4, NULL, 5, NULL),		-- tenant 4: a group, and a group nobody is in
	(12, 4, NULL, 7, NULL),
	(13, 5, 40, NULL, NULL),	-- tenant 5: a member and a group that has it
	(14, 5, NULL, 5, NULL),
	(15, 6, 50, 3, NULL);		-- tenant 6: one row that is a direct and a joined grant at once
INSERT INTO facts SELECT i, CASE WHEN i % 17 = 0 THEN NULL WHEN i % 19 = 0 THEN 100000 + i ELSE 1 + (i * 7) % 60 END, timestamptz '2026-01-01' + i * interval '9 minutes', i % 10
	FROM generate_series(1, 3000) i;
CREATE VIEW pair_oracle AS
	SELECT dim_a_id AS member, tenant_id FROM grants WHERE deleted_at IS NULL AND dim_a_id IS NOT NULL
	UNION
	SELECT m.dim_a_id, g.tenant_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL;

CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, amount)
	WITH (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)');
-- built before the journals existed: the stamps are the next build's
REINDEX INDEX facts_col;
VACUUM (FREEZE, ANALYZE) facts;
VACUUM (ANALYZE) grants, memberships;

-- the shapes, each over the keys 1..6 one at a time and over the keys {1..6} and {2, 5} together: the marker of the scope (once, or a row for each pair) and the plain statement it stands for
CREATE TABLE shapes (n int, label text, q text, plain text);
INSERT INTO shapes VALUES
	(1, 'IN', 'SELECT count(*), sum(amount) FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''tenant_id'', f.dim_a_id, KEYS)',
		'SELECT count(*), sum(amount) FROM facts f WHERE f.dim_a_id IN (SELECT member FROM pair_oracle WHERE tenant_id IN LIST)'),
	(2, 'EXISTS', 'SELECT count(*), sum(amount) FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''tenant_id'', f.dim_a_id, KEYS)',
		'SELECT count(*), sum(amount) FROM facts f WHERE EXISTS (SELECT 1 FROM pair_oracle o WHERE o.member = f.dim_a_id AND o.tenant_id IN LIST)'),
	(3, 'GROUP BY', 'SELECT amount, count(*) FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''tenant_id'', f.dim_a_id, KEYS) GROUP BY amount ORDER BY amount',
		'SELECT amount, count(*) FROM facts f WHERE f.dim_a_id IN (SELECT member FROM pair_oracle WHERE tenant_id IN LIST) GROUP BY amount ORDER BY amount'),
	(4, 'join, a row for each pair', 'SELECT count(*), sum(amount) FROM facts f WHERE sift.in_scope_pairs(''facts_col''::regclass, ''tenant_id'', f.dim_a_id, KEYS)',
		'SELECT count(*), sum(amount) FROM facts f JOIN pair_oracle o ON o.member = f.dim_a_id WHERE o.tenant_id IN LIST'),
	(5, 'top-N', 'SELECT id, created_at FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''tenant_id'', f.dim_a_id, KEYS) ORDER BY created_at DESC, id DESC LIMIT 20',
		'SELECT id, created_at FROM facts f WHERE f.dim_a_id IN (SELECT member FROM pair_oracle WHERE tenant_id IN LIST) ORDER BY created_at DESC, id DESC LIMIT 20');
CREATE TABLE key_sets (keys text, list text);
INSERT INTO key_sets SELECT k::text, '(' || k || ')' FROM generate_series(1, 6) k;
INSERT INTO key_sets VALUES ('ARRAY[1, 2, 3, 4, 5, 6]::int8[]', '(1, 2, 3, 4, 5, 6)'), ('ARRAY[2, 5]::int8[]', '(2, 5)');
CREATE TABLE reads AS SELECT s.n, s.label, replace(s.q, 'KEYS', k.keys) AS q, replace(s.plain, 'LIST', k.list) AS plain FROM shapes s, key_sets k;

-- a read of the scope against its plain statement (scope_run of test/scope_helpers.sql also runs the marker as a row filter, which counts a row once: not the join of a statement that counts each pair).
-- A read of several keys that counts each pair and has a patched member in two of them has no entity path: it is refused with its reason, which is a documented one
CREATE FUNCTION read_run(q text, plain text, OUT same boolean, OUT stored boolean, OUT why text, OUT patched boolean, OUT path_ok boolean) LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	a text;
	b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	BEGIN
		EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
		EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO a;
	EXCEPTION WHEN feature_not_supported THEN
		why := substring(SQLERRM FROM '"(.*)"$');
		path_ok := scope_reason_ok(why);
		stored := false;
		patched := false;
		RETURN;
	END;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || plain || ') t' INTO b;
	SET LOCAL sift.enable_agg = on;
	same := a = b;
	why := jsonb_path_query_first(p, '$.**."Scope Fallback"') #>> '{}';
	stored := jsonb_path_exists(p, '$.**."Scope Units Read"');
	patched := coalesce((jsonb_path_query_first(p, '$.**."Scope Patched Keys"') #>> '{}')::int, 0) > 0;
	path_ok := stored OR scope_reason_ok(why);
END $$;

-- one line for all the reads: how many, how many differ from the plain statement, how many took the units, how many patched members, how many were refused for the join, how many are not
-- accounted for (neither the units nor a documented reason), and the reasons
CREATE FUNCTION verdict(OUT reads int, OUT differ int, OUT units int, OUT patched int, OUT refused int, OUT unaccounted int, OUT reasons text) LANGUAGE sql AS $$
	SELECT count(*)::int, count(*) FILTER (WHERE NOT r.same)::int, count(*) FILTER (WHERE r.stored)::int, count(*) FILTER (WHERE r.patched)::int, count(*) FILTER (WHERE r.same IS NULL)::int,
		count(*) FILTER (WHERE NOT r.path_ok)::int, coalesce(string_agg(DISTINCT r.why, ', '), '-')
	FROM reads, LATERAL read_run(reads.q, reads.plain) r
$$;
-- the members whose pairs changed since the stamps of the scope segments, as the journals tell them
CREATE FUNCTION changed(OUT why text, OUT dim_b int8[]) LANGUAGE sql AS $$
	SELECT min(c.why), array_agg(c.key ORDER BY c.key) FILTER (WHERE c.key IS NOT NULL)
	FROM (SELECT * FROM sift._test_col_scope_journal_stamps('facts_col') WHERE family = 'scope' AND journal = 0 LIMIT 1) a,
		(SELECT * FROM sift._test_col_scope_journal_stamps('facts_col') WHERE family = 'scope' AND journal = 1 LIMIT 1) b,
		(SELECT * FROM sift._test_col_scope_journal_stamps('facts_col') WHERE family = 'scope' AND journal = 2 LIMIT 1) c3,
		LATERAL sift._test_scope_journal_changes('facts_col', 0, a.lineage, a.horizon, 1000000, b.lineage, c3.lineage) c
$$;

-- the stamps: one for each of the three journals, in every segment, from the lineage of its journal index
SELECT journal, count(DISTINCT lineage) AS lineages, count(DISTINCT horizon::text) AS horizons, count(*) AS segments FROM sift._test_col_scope_journal_stamps('facts_col') GROUP BY journal ORDER BY journal;
SELECT format AS column_format, count(*) AS segments FROM sift.col_segments('facts_col') WHERE kind = 'immutable' GROUP BY 1;
SELECT 'built' AS state, c.*, v.* FROM changed() c, verdict() v;

-- each state: what it does first (the stamps are taken after it), the edit, the members the journals say changed, and the reads
CREATE TABLE states (n int, label text, setup text, edit text);
INSERT INTO states VALUES
	(1, 'a grant to a member', NULL, 'INSERT INTO grants VALUES (101, 3, 30, NULL, NULL)'),
	(2, 'a grant to a member soft deleted', NULL, 'UPDATE grants SET deleted_at = now() WHERE id = 7'),
	(3, 'a grant to a member restored', 'UPDATE grants SET deleted_at = now() WHERE id = 9', 'UPDATE grants SET deleted_at = NULL WHERE id = 9'),
	(4, 'a grant to a member deleted', NULL, 'DELETE FROM grants WHERE id = 8'),
	(5, 'a grant moved from a member to a group', NULL, 'UPDATE grants SET dim_a_id = NULL, group_id = 6 WHERE id = 13'),
	(6, 'a grant moved from a group to a member', NULL, 'UPDATE grants SET group_id = NULL, dim_a_id = 41 WHERE id = 14'),
	(7, 'a grant to a group', NULL, 'INSERT INTO grants VALUES (102, 2, NULL, 1, NULL)'),
	(8, 'a grant to a group soft deleted', NULL, 'UPDATE grants SET deleted_at = now() WHERE id = 5'),
	(9, 'a grant to a group restored', NULL, 'UPDATE grants SET deleted_at = NULL WHERE id = 10'),
	(10, 'a membership', NULL, 'INSERT INTO memberships (group_id, dim_a_id) VALUES (3, 59)'),
	(11, 'a membership soft deleted', NULL, 'UPDATE memberships SET deleted_at = now() WHERE group_id = 3 AND dim_a_id = 8'),
	(12, 'a membership restored', NULL, 'UPDATE memberships SET deleted_at = NULL WHERE group_id = 6 AND dim_a_id = 11'),
	(13, 'a member moved to another group', NULL, 'UPDATE memberships SET group_id = 5 WHERE group_id = 3 AND dim_a_id = 14'),
	(14, 'a member reached by two paths loses one', NULL, 'UPDATE grants SET deleted_at = now() WHERE id = 1'),
	(15, 'a member reached by two paths loses both', NULL, 'UPDATE memberships SET deleted_at = now() WHERE dim_a_id = 5; UPDATE grants SET deleted_at = NULL WHERE id = 1; UPDATE grants SET deleted_at = now() WHERE id = 1'),
	(16, 'a grant to a group deleted and added again', NULL, 'DELETE FROM grants WHERE id = 3; INSERT INTO grants VALUES (103, 1, NULL, 6, NULL)'),
	(17, 'a group emptied and filled again', NULL, 'WITH d AS (DELETE FROM memberships WHERE group_id = 2 RETURNING dim_a_id) INSERT INTO memberships (group_id, dim_a_id) SELECT 2, dim_a_id FROM d'),
	(18, 'a column no btree holds (HOT)', NULL, 'UPDATE grants SET note = ''n'' WHERE id IN (2, 4, 6); UPDATE memberships SET note = ''n'' WHERE group_id = 6'),
	(19, 'a rolled back transaction', NULL, 'BEGIN; INSERT INTO grants VALUES (104, 5, 20, NULL, NULL); UPDATE memberships SET deleted_at = now() WHERE group_id = 4; DELETE FROM grants WHERE id = 4; ROLLBACK'),
	(20, 'a rolled back savepoint', NULL, 'BEGIN; SAVEPOINT s; UPDATE memberships SET group_id = 6 WHERE group_id = 4 AND dim_a_id = 3; INSERT INTO grants VALUES (105, 6, NULL, 4, NULL); ROLLBACK TO s; COMMIT'),
	(21, 'a released savepoint', NULL, 'BEGIN; SAVEPOINT s; INSERT INTO grants VALUES (106, 6, NULL, 4, NULL); RELEASE s; COMMIT'),
	(22, 'a statement that fails', NULL, 'INSERT INTO grants VALUES (107, 3, 31, NULL, NULL), (108, 3, 31, NULL, NULL)'),
	(23, 'a grant to a group of dim_b that are also granted directly', NULL, 'INSERT INTO grants VALUES (109, 1, NULL, 1, NULL)');
SELECT stmt FROM (SELECT n, 0 AS k, setup AS stmt FROM states WHERE setup IS NOT NULL
	UNION ALL SELECT n, 1, 'REINDEX INDEX facts_col' FROM states
	UNION ALL SELECT n, 2, edit FROM states
	UNION ALL SELECT n, 3, format('SELECT %s AS n, %L AS state, c.*, v.* FROM changed() c, verdict() v', n, label) FROM states) t WHERE stmt IS NOT NULL ORDER BY n, k \gexec

-- the edits of the whole run are in the pairs now: a rebuild has nothing to patch
REINDEX INDEX facts_col;
SELECT 'rebuilt' AS state, c.*, v.* FROM changed() c, verdict() v;

-- the changes of this transaction are in its own reads before it commits, and gone with a rollback
BEGIN;
DELETE FROM memberships WHERE group_id = 6;
INSERT INTO grants VALUES (110, 2, 33, NULL, NULL);
SELECT 'own transaction' AS state, c.*, v.* FROM changed() c, verdict() v;
ROLLBACK;
SELECT 'after the rollback' AS state, c.*, v.* FROM changed() c, verdict() v;

-- facts written after the build are in a write buffer, then in the scope segments of a promotion (the pairs of the relations as they are then), and a merge keeps the stamps: with edits since
INSERT INTO facts SELECT i, 1 + (i * 7) % 60, timestamptz '2026-03-01' + i * interval '9 minutes', i % 10 FROM generate_series(3001, 3400) i;
UPDATE memberships SET group_id = 4 WHERE group_id = 5 AND dim_a_id = 41;
SELECT 'facts in a write buffer' AS state, c.*, v.* FROM changed() c, verdict() v;
SELECT sift.col_promote('facts_col') >= 0 AS promoted;
SELECT 'promoted' AS state, c.*, v.* FROM changed() c, verdict() v;
UPDATE grants SET deleted_at = now() WHERE id = 14;
SELECT sift.col_merge('facts_col', true) >= 0 AS merged;
SELECT 'merged' AS state, c.*, v.* FROM changed() c, verdict() v;
SELECT journal, count(DISTINCT lineage) AS lineages, count(DISTINCT horizon::text) AS horizons FROM sift._test_col_scope_journal_stamps('facts_col') GROUP BY journal ORDER BY journal;
SELECT sift.col_verify('facts_col', true);

-- workers allowed and the price of a Gather at zero: a scope read is decided once, by the leader (the verdict, the changed members and the patch are made from the snapshot of the statement), so a read with
-- the patch of each of the three journals is one process, and answers as the join
SET max_parallel_workers_per_gather = 4;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
CREATE TABLE parallel_states (n int, label text, edit text);
INSERT INTO parallel_states VALUES
	(1, 'a grant to a member, workers allowed', 'INSERT INTO grants VALUES (201, 5, 33, NULL, NULL)'),
	(2, 'a grant to a group, workers allowed', 'INSERT INTO grants VALUES (202, 5, NULL, 1, NULL)'),
	(3, 'a membership soft deleted, workers allowed', 'UPDATE memberships SET deleted_at = now() WHERE group_id = 2 AND dim_a_id = 1'),
	(4, 'the three at once, workers allowed', 'INSERT INTO grants VALUES (203, 6, 34, NULL, NULL); INSERT INTO grants VALUES (204, 6, NULL, 5, NULL); UPDATE memberships SET deleted_at = NULL WHERE group_id = 2 AND dim_a_id = 1');
SELECT stmt FROM (SELECT n, 0 AS k, 'REINDEX INDEX facts_col' AS stmt FROM parallel_states
	UNION ALL SELECT n, 1, edit FROM parallel_states
	UNION ALL SELECT n, 2, format('SELECT %s AS n, %L AS state, c.*, v.* FROM changed() c, verdict() v', n, label) FROM parallel_states) t ORDER BY n, k \gexec
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
SELECT sift.col_verify('facts_col', true);

DROP TABLE facts, grants, memberships, shapes, key_sets, reads, states, parallel_states CASCADE;
DROP FUNCTION verdict(), changed(), read_run(text, text), scope_run(text, text), scope_group_run(text, text), scope_reason_ok(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
