-- shared by col_scope_texttwohop and col_scope_texttwohop_collide: a scope of two paths (the grants of a tenant on a member and on the groups of the member) with a text key, a text member, a uuid member
-- and a varchar fact column, the forms of its pair set and the states of the three journals
CREATE TABLE grants (id int PRIMARY KEY, tenant_id text NOT NULL, account text, group_id int, deleted_at timestamptz, note text) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX grants_direct ON grants (account, tenant_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX grants_group ON grants (group_id, tenant_id) WHERE deleted_at IS NULL;
CREATE TABLE memberships (id serial PRIMARY KEY, group_id int NOT NULL, account text NOT NULL, deleted_at timestamptz, note text) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX memberships_active ON memberships (group_id, account) WHERE deleted_at IS NULL;
CREATE INDEX grants_by_account ON grants USING sift_col (account, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX grants_by_group ON grants USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX memberships_by_account ON memberships USING sift_col (account, group_id) WITH (track_changes = 'ctid');

-- accounts 'acct-1' to 'acct-58' are in the group 1 + n % 6, every fifth in a second group, every eleventh of them soft deleted from the first; two accounts differing in the bytes only, the empty one
INSERT INTO memberships (group_id, account, deleted_at) SELECT 1 + m % 6, 'acct-' || m, CASE WHEN m % 11 = 0 THEN now() END FROM generate_series(1, 58) m;
INSERT INTO memberships (group_id, account) SELECT 1 + (m + 2) % 6, 'acct-' || m FROM generate_series(5, 58, 5) m;
INSERT INTO memberships (group_id, account) VALUES (3, 'Acct-5'), (3, 'acct-5 '), (4, '');
INSERT INTO grants (id, tenant_id, account, group_id, deleted_at) VALUES
	(1, 'tenant-1', 'acct-5', NULL, NULL),		-- tenant 1: acct-5 directly, and through its groups (6 and 2)
	(2, 'tenant-1', 'acct-17', NULL, NULL),
	(3, 'tenant-1', NULL, 6, NULL),
	(4, 'tenant-1', NULL, 2, NULL),
	(5, 'tenant-2', NULL, 3, NULL),				-- tenant 2: a group (with the accounts that differ in the bytes), and a soft deleted direct grant
	(6, 'tenant-2', 'acct-8', NULL, now()),
	(7, 'tenant-3', 'acct-1', NULL, NULL),		-- tenant 3: three accounts, the empty one, and a soft deleted group
	(8, 'tenant-3', 'acct-2', NULL, NULL),
	(9, 'tenant-3', '', NULL, NULL),
	(10, 'tenant-3', NULL, 4, now()),
	(11, 'tenant-4', NULL, 5, NULL),			-- tenant 4: a group, and a group nobody is in
	(12, 'tenant-4', NULL, 7, NULL),
	(13, 'tenant-5', 'acct-40', NULL, NULL),	-- tenant 5: an account and a group that has it
	(14, 'tenant-5', NULL, 5, NULL),
	(15, 'tenant-6', 'acct-50', 3, NULL);		-- tenant 6: one row that is a direct and a joined grant at once
CREATE TABLE facts (id int PRIMARY KEY, account text, created_at timestamptz, kind text, amount int) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, CASE WHEN i % 17 = 0 THEN NULL WHEN i % 19 = 0 THEN 'other-' || i WHEN i % 23 = 0 THEN '' WHEN i % 29 = 0 THEN 'Acct-5'
	WHEN i % 31 = 0 THEN 'acct-5 ' ELSE 'acct-' || (1 + (i * 7) % 60) END, timestamptz '2026-01-01' + i * interval '9 minutes', 'k' || i % 4, i % 10 FROM generate_series(1, 3000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, account, created_at, kind, amount)
	WITH (scope = 'tenant_id: account -> grants(account, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, account)');
REINDEX INDEX facts_col;

-- a varchar fact column, a uuid member with a uuid key (the same pairs through md5), and an integer key with a text member
CREATE TABLE var_facts (id int PRIMARY KEY, account varchar(2000), kind text, amount int) WITH (autovacuum_enabled = false);
INSERT INTO var_facts SELECT id, account, kind, amount FROM facts;
CREATE INDEX var_facts_col ON var_facts USING sift_col (id, account, kind, amount)
	WITH (scope = 'tenant_id: account -> grants(account, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, account)');
REINDEX INDEX var_facts_col;
CREATE TABLE u_grants (id int PRIMARY KEY, tenant_id uuid NOT NULL, account uuid, group_id int, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX u_grants_direct ON u_grants (account, tenant_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX u_grants_group ON u_grants (group_id, tenant_id) WHERE deleted_at IS NULL;
CREATE TABLE u_memberships (id serial PRIMARY KEY, group_id int NOT NULL, account uuid NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX u_memberships_active ON u_memberships (group_id, account) WHERE deleted_at IS NULL;
INSERT INTO u_grants SELECT id, md5(tenant_id)::uuid, md5(account)::uuid, group_id, deleted_at FROM grants;
INSERT INTO u_memberships SELECT id, group_id, md5(account)::uuid, deleted_at FROM memberships;
CREATE INDEX u_grants_by_account ON u_grants USING sift_col (account, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX u_grants_by_group ON u_grants USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX u_memberships_by_account ON u_memberships USING sift_col (account, group_id) WITH (track_changes = 'ctid');
CREATE TABLE u_facts (id int PRIMARY KEY, account uuid, kind text, amount int) WITH (autovacuum_enabled = false);
INSERT INTO u_facts SELECT id, md5(account)::uuid, kind, amount FROM facts;
CREATE INDEX u_facts_col ON u_facts USING sift_col (id, account, kind, amount)
	WITH (scope = 'tenant_id: account -> u_grants(account, tenant_id) | u_grants(group_id, tenant_id) via u_memberships(group_id, account)');
REINDEX INDEX u_facts_col;
CREATE TABLE i_grants (id int PRIMARY KEY, tenant_id int NOT NULL, account text, group_id int, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX i_grants_direct ON i_grants (account, tenant_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX i_grants_group ON i_grants (group_id, tenant_id) WHERE deleted_at IS NULL;
INSERT INTO i_grants SELECT id, substr(tenant_id, 8)::int, account, group_id, deleted_at FROM grants;
CREATE INDEX i_grants_by_account ON i_grants USING sift_col (account, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX i_grants_by_group ON i_grants USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE i_facts (id int PRIMARY KEY, account text, kind text, amount int) WITH (autovacuum_enabled = false);
INSERT INTO i_facts SELECT id, account, kind, amount FROM facts;
CREATE INDEX i_facts_col ON i_facts USING sift_col (id, account, kind, amount)
	WITH (scope = 'tenant_id: account -> i_grants(account, tenant_id) | i_grants(group_id, tenant_id) via memberships(group_id, account)');
REINDEX INDEX i_facts_col;
VACUUM (FREEZE, ANALYZE) facts, var_facts, u_facts, i_facts;
VACUUM (ANALYZE) grants, memberships, u_grants, u_memberships, i_grants;

-- the scope segments of an index with a text key or member are column format 50
SELECT i AS index, format, count(*) AS segments FROM (VALUES ('facts_col'), ('var_facts_col'), ('u_facts_col'), ('i_facts_col')) v (i), LATERAL sift.col_segments(i::regclass) GROUP BY 1, 2 ORDER BY 1, 2;
SELECT count(*) AS entries, count(DISTINCT scope) AS scopes FROM sift._test_col_scope_entries('facts_col');

-- the pair set as the statements of an application write it: the union of the direct grants and the members of the groups granted
CREATE VIEW pair_oracle AS
	SELECT g.account AS member, g.tenant_id FROM grants g WHERE g.deleted_at IS NULL AND g.account IS NOT NULL
	UNION
	SELECT m.account, g.tenant_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL;
CREATE VIEW u_pair_oracle AS
	SELECT g.account AS member, g.tenant_id FROM u_grants g WHERE g.deleted_at IS NULL AND g.account IS NOT NULL
	UNION
	SELECT m.account, g.tenant_id FROM u_grants g JOIN u_memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL;
CREATE VIEW i_pair_oracle AS
	SELECT g.account AS member, g.tenant_id FROM i_grants g WHERE g.deleted_at IS NULL AND g.account IS NOT NULL
	UNION
	SELECT m.account, g.tenant_id FROM i_grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL;

CREATE TABLE forms (name text PRIMARY KEY, q text);
INSERT INTO forms VALUES
 ('IN, one key', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE f.account IN (SELECT member FROM pair_oracle WHERE tenant_id = 'tenant-1') GROUP BY kind$q$),
 ('IN, a list', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE f.account IN (SELECT member FROM pair_oracle WHERE tenant_id IN ('tenant-1', 'tenant-2', 'tenant-5')) GROUP BY kind$q$),
 ('IN, a key nobody has', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE f.account IN (SELECT member FROM pair_oracle WHERE tenant_id = 'tenant-99') GROUP BY kind$q$),
 ('IN, the group with the bytes that differ', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE f.account IN (SELECT member FROM pair_oracle WHERE tenant_id = 'tenant-2') GROUP BY kind$q$),
 ('IN, the key with the empty account', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE f.account IN (SELECT member FROM pair_oracle WHERE tenant_id = 'tenant-3') GROUP BY kind$q$),
 ('EXISTS', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE EXISTS (SELECT 1 FROM pair_oracle o WHERE o.member = f.account AND o.tenant_id = 'tenant-1') GROUP BY kind$q$),
 ('join, a row for each pair', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f JOIN pair_oracle o ON o.member = f.account WHERE o.tenant_id IN ('tenant-1', 'tenant-5') GROUP BY kind$q$),
 ('group by the key, every key', $q$SELECT o.tenant_id, count(*) AS n, sum(amount) AS total FROM facts f JOIN pair_oracle o ON o.member = f.account GROUP BY o.tenant_id$q$),
 ('every fact with a pair', $q$SELECT count(*) AS n, sum(amount) AS total FROM facts f WHERE EXISTS (SELECT 1 FROM pair_oracle o WHERE o.member = f.account)$q$),
 ('varchar facts', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM var_facts f WHERE f.account IN (SELECT member FROM pair_oracle WHERE tenant_id IN ('tenant-1', 'tenant-5')) GROUP BY kind$q$),
 ('uuid member and key', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM u_facts f WHERE f.account IN (SELECT member FROM u_pair_oracle WHERE tenant_id IN (md5('tenant-1')::uuid, md5('tenant-5')::uuid)) GROUP BY kind$q$),
 ('uuid member and key, group by the key', $q$SELECT o.tenant_id, count(*) AS n FROM u_facts f JOIN u_pair_oracle o ON o.member = f.account GROUP BY o.tenant_id$q$),
 ('integer key, text member', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM i_facts f WHERE f.account IN (SELECT member FROM i_pair_oracle WHERE tenant_id IN (1, 5)) GROUP BY kind$q$);

CREATE FUNCTION label(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text; plan text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		plan := plan || l || E'\n';
	END LOOP;
	IF plan ~ 'Scope Index: ' THEN
		RETURN 'scope read, ' || substring(plan from 'Scope Semantics: ([^\n]*)');
	END IF;
	SELECT regexp_replace(reason, '^.*the join runs as written \((.*)\)$', '\1') INTO r FROM sift.explain_why(q) WHERE arm = 'scope';
	RETURN CASE WHEN r IS NOT NULL THEN 'join, ' || r WHEN plan ~ 'Lookup Joins' THEN 'join' WHEN plan ~ 'SiftAgg' THEN 'SiftAgg' ELSE 'normal plan' END;
END $$;
CREATE FUNCTION rows_md5(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	DROP TABLE IF EXISTS scope_rows;
	EXECUTE 'CREATE TEMP TABLE scope_rows AS ' || q;
	SELECT md5(coalesce(string_agg(t::text, '|' ORDER BY t::text), '')) INTO r FROM scope_rows t;
	DROP TABLE scope_rows;
	RETURN r;
END $$;
CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text; c text;
BEGIN
	a := rows_md5(q);
	PERFORM set_config('sift.agg_scope', 'off', false);
	b := rows_md5(q);
	PERFORM set_config('sift.enable_agg', 'off', false);
	c := rows_md5(q);
	PERFORM set_config('sift.agg_scope', 'on', false);
	PERFORM set_config('sift.enable_agg', 'on', false);
	RETURN a = b AND b = c;
END $$;
CREATE FUNCTION run_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Scope (Fallback|Changed Keys|Patched Keys|Dedupe Keys|Members Listed)' THEN
			r := r || regexp_replace(l, '^ *Scope ', '') || '; ';
		END IF;
	END LOOP;
	RETURN CASE WHEN r = '' THEN 'units' ELSE rtrim(r, '; ') END;
END $$;
CREATE FUNCTION states() RETURNS TABLE (name text, same_rows bool) LANGUAGE sql AS $$
	SELECT f.name, same(f.q) FROM forms f ORDER BY f.name COLLATE "C"
$$;

SELECT name, label(q) AS plan, same(q) AS same_rows FROM forms ORDER BY name COLLATE "C";
SELECT name, run_of(q) AS run FROM forms ORDER BY name COLLATE "C";

-- the grants, memberships and facts are mirrored to the uuid, varchar and integer relations by id, writing only the rows that differ
CREATE FUNCTION mirror() RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	DELETE FROM u_grants u WHERE NOT EXISTS (SELECT 1 FROM grants g WHERE g.id = u.id);
	INSERT INTO u_grants SELECT id, md5(tenant_id)::uuid, md5(account)::uuid, group_id, deleted_at FROM grants g WHERE NOT EXISTS (SELECT 1 FROM u_grants u WHERE g.id = u.id);
	UPDATE u_grants u SET tenant_id = md5(g.tenant_id)::uuid, account = md5(g.account)::uuid, group_id = g.group_id, deleted_at = g.deleted_at FROM grants g
		WHERE g.id = u.id AND (u.tenant_id, u.account, u.group_id, u.deleted_at) IS DISTINCT FROM (md5(g.tenant_id)::uuid, md5(g.account)::uuid, g.group_id, g.deleted_at);
	DELETE FROM u_memberships u WHERE NOT EXISTS (SELECT 1 FROM memberships m WHERE m.id = u.id);
	INSERT INTO u_memberships SELECT id, group_id, md5(account)::uuid, deleted_at FROM memberships m WHERE NOT EXISTS (SELECT 1 FROM u_memberships u WHERE m.id = u.id);
	UPDATE u_memberships u SET group_id = m.group_id, account = md5(m.account)::uuid, deleted_at = m.deleted_at FROM memberships m
		WHERE m.id = u.id AND (u.group_id, u.account, u.deleted_at) IS DISTINCT FROM (m.group_id, md5(m.account)::uuid, m.deleted_at);
	DELETE FROM i_grants i WHERE NOT EXISTS (SELECT 1 FROM grants g WHERE g.id = i.id);
	INSERT INTO i_grants SELECT id, substr(tenant_id, 8)::int, account, group_id, deleted_at FROM grants g WHERE NOT EXISTS (SELECT 1 FROM i_grants i WHERE g.id = i.id);
	UPDATE i_grants i SET tenant_id = substr(g.tenant_id, 8)::int, account = g.account, group_id = g.group_id, deleted_at = g.deleted_at FROM grants g
		WHERE g.id = i.id AND (i.tenant_id, i.account, i.group_id, i.deleted_at) IS DISTINCT FROM (substr(g.tenant_id, 8)::int, g.account, g.group_id, g.deleted_at);
END $$;
CREATE FUNCTION mirror_facts() RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	DELETE FROM var_facts v WHERE NOT EXISTS (SELECT 1 FROM facts f WHERE f.id = v.id);
	INSERT INTO var_facts SELECT id, account, kind, amount FROM facts f WHERE NOT EXISTS (SELECT 1 FROM var_facts v WHERE f.id = v.id);
	UPDATE var_facts v SET account = f.account FROM facts f WHERE f.id = v.id AND v.account IS DISTINCT FROM f.account;
	DELETE FROM u_facts v WHERE NOT EXISTS (SELECT 1 FROM facts f WHERE f.id = v.id);
	INSERT INTO u_facts SELECT id, md5(account)::uuid, kind, amount FROM facts f WHERE NOT EXISTS (SELECT 1 FROM u_facts v WHERE f.id = v.id);
	UPDATE u_facts v SET account = md5(f.account)::uuid FROM facts f WHERE f.id = v.id AND v.account IS DISTINCT FROM md5(f.account)::uuid;
	DELETE FROM i_facts v WHERE NOT EXISTS (SELECT 1 FROM facts f WHERE f.id = v.id);
	INSERT INTO i_facts SELECT id, account, kind, amount FROM facts f WHERE NOT EXISTS (SELECT 1 FROM i_facts v WHERE f.id = v.id);
	UPDATE i_facts v SET account = f.account FROM facts f WHERE f.id = v.id AND v.account IS DISTINCT FROM f.account;
END $$;

-- a state: the edit of the pairs (the mirrors follow), the forms against the join, and what the run did
CREATE TABLE edits (n int, label text, edit text);
INSERT INTO edits VALUES
 (1, 'a grant to an account', $e$INSERT INTO grants VALUES (101, 'tenant-3', 'acct-30', NULL, NULL)$e$),
 (2, 'a grant to an account no fact has', $e$INSERT INTO grants VALUES (102, 'tenant-1', 'acct-nobody', NULL, NULL)$e$),
 (3, 'a grant to an account soft deleted', $e$UPDATE grants SET deleted_at = now() WHERE id = 7$e$),
 (4, 'a grant to an account deleted', $e$DELETE FROM grants WHERE id = 8$e$),
 (5, 'a grant to a group', $e$INSERT INTO grants VALUES (103, 'tenant-2', NULL, 1, NULL)$e$),
 (6, 'a grant to a group soft deleted', $e$UPDATE grants SET deleted_at = now() WHERE id = 5$e$),
 (7, 'a grant moved from an account to a group', $e$UPDATE grants SET account = NULL, group_id = 6 WHERE id = 13$e$),
 (8, 'a grant moved to another key', $e$UPDATE grants SET tenant_id = 'tenant-7' WHERE id = 2$e$),
 (9, 'a membership', $e$INSERT INTO memberships (group_id, account) VALUES (3, 'acct-59'), (3, 'acct-new')$e$),
 (10, 'a membership soft deleted', $e$UPDATE memberships SET deleted_at = now() WHERE group_id = 3 AND account = 'acct-8'$e$),
 (11, 'an account moved to another group', $e$UPDATE memberships SET group_id = 5 WHERE group_id = 3 AND account = 'acct-14'$e$),
 (12, 'an account renamed', $e$UPDATE memberships SET account = 'acct-renamed' WHERE group_id = 3 AND account = 'Acct-5'$e$),
 (13, 'an account reached by two paths loses one', $e$UPDATE grants SET deleted_at = now() WHERE id = 1$e$),
 (14, 'an account reached by two paths loses both', $e$UPDATE memberships SET deleted_at = now() WHERE account = 'acct-5'$e$),
 (15, 'a group emptied and filled again', $e$WITH d AS (DELETE FROM memberships WHERE group_id = 2 RETURNING account) INSERT INTO memberships (group_id, account) SELECT 2, account FROM d$e$),
 (16, 'a column no btree holds (HOT)', $e$UPDATE grants SET note = 'n' WHERE id IN (2, 4, 6); UPDATE memberships SET note = 'n' WHERE group_id = 6$e$),
 (17, 'a rolled back transaction', $e$BEGIN; INSERT INTO grants VALUES (104, 'tenant-5', 'acct-20', NULL, NULL); UPDATE memberships SET deleted_at = now() WHERE group_id = 4; ROLLBACK$e$),
 (18, 'the empty account in a group', $e$INSERT INTO grants VALUES (105, 'tenant-4', NULL, 4, NULL)$e$);
SELECT stmt FROM (SELECT n, 0 AS k, 'REINDEX INDEX facts_col; REINDEX INDEX var_facts_col; REINDEX INDEX u_facts_col; REINDEX INDEX i_facts_col' AS stmt FROM edits
	UNION ALL SELECT n, 1, edit FROM edits
	UNION ALL SELECT n, 2, 'SELECT mirror()' FROM edits
	UNION ALL SELECT n, 3, format('SELECT %s AS n, %L AS state, count(*) FILTER (WHERE NOT same_rows) AS differing, count(*) AS forms FROM states()', n, label) FROM edits) t ORDER BY n, k \gexec
SELECT name, run_of(q) AS run FROM forms WHERE name IN ('IN, one key', 'IN, a list', 'join, a row for each pair', 'uuid member and key', 'integer key, text member') ORDER BY name COLLATE "C";

-- the facts change: re-assigned accounts, no account, the empty one, new facts, the whole table written again
UPDATE facts SET account = 'acct-7' WHERE id % 50 = 0;
UPDATE facts SET account = NULL WHERE id % 61 = 0;
UPDATE facts SET account = '' WHERE id % 73 = 0;
INSERT INTO facts SELECT 3100 + i, 'acct-' || (1 + i % 58), timestamptz '2026-06-01', 'k0', 5 FROM generate_series(1, 300) i;
SELECT mirror_facts();
SELECT 'facts changed' AS state, count(*) FILTER (WHERE NOT same_rows) AS differing, count(*) AS forms FROM states();
SELECT name, run_of(q) AS run FROM forms WHERE name IN ('IN, one key', 'IN, a list', 'varchar facts') ORDER BY name COLLATE "C";

-- keys the value store has never seen get a code of the statement: two such keys in two statements are not the same key (what a backend keeps of the groups of its keys is not shared by them)
REINDEX INDEX facts_col;
REINDEX INDEX u_facts_col;
INSERT INTO grants VALUES (201, 'tenant-11', NULL, 3, NULL), (202, 'tenant-12', NULL, 5, NULL), (203, 'tenant-11', 'acct-9', NULL, NULL);
INSERT INTO memberships (group_id, account) VALUES (3, 'acct-new-a'), (5, 'acct-new-b'), (3, 'acct-11');
INSERT INTO facts SELECT 5000 + i, CASE i % 3 WHEN 0 THEN 'acct-new-a' WHEN 1 THEN 'acct-new-b' ELSE 'acct-11' END, timestamptz '2026-07-01', 'k1', 3 FROM generate_series(1, 30) i;
SELECT mirror();
SELECT mirror_facts();
SELECT k AS key, same(format($q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE f.account IN (SELECT member FROM pair_oracle WHERE tenant_id IN ('tenant-1', %L)) GROUP BY kind$q$, k)) AS same_rows,
       same(format($q$SELECT kind, sum(amount) AS total, count(*) AS n FROM u_facts f WHERE f.account IN (SELECT member FROM u_pair_oracle WHERE tenant_id IN (md5('tenant-1')::uuid, md5(%L)::uuid)) GROUP BY kind$q$, k)) AS same_uuid
FROM unnest(ARRAY['tenant-11', 'tenant-12', 'tenant-11', 'tenant-13', 'tenant-12']) WITH ORDINALITY t (k, n) ORDER BY n;

-- the journals are merged into the index: vacuum, a rebuild, a promotion and a merge
VACUUM facts, var_facts, u_facts, i_facts, grants, memberships, u_grants, u_memberships, i_grants;
SELECT 'vacuumed' AS state, count(*) FILTER (WHERE NOT same_rows) AS differing, count(*) AS forms FROM states();
REINDEX INDEX facts_col;
REINDEX INDEX var_facts_col;
REINDEX INDEX u_facts_col;
REINDEX INDEX i_facts_col;
SELECT 'reindexed' AS state, count(*) FILTER (WHERE NOT same_rows) AS differing, count(*) AS forms FROM states();
SELECT name, run_of(q) AS run FROM forms ORDER BY name COLLATE "C";
SELECT sift.col_promote('facts_col') >= 0 AS promoted;
UPDATE grants SET deleted_at = now() WHERE id = 14;
SELECT mirror();
SELECT sift.col_merge('facts_col', true) >= 0 AS merged;
SELECT 'promoted and merged' AS state, count(*) FILTER (WHERE NOT same_rows) AS differing, count(*) AS forms FROM states();

SELECT sift.col_verify('facts_col', true), sift.col_verify('var_facts_col', true), sift.col_verify('u_facts_col', true), sift.col_verify('i_facts_col', true);

DROP FUNCTION states(), same(text), rows_md5(text), run_of(text), label(text), mirror(), mirror_facts();
DROP TABLE forms, edits, facts, var_facts, u_facts, i_facts, grants, memberships, u_grants, u_memberships, i_grants CASCADE;
