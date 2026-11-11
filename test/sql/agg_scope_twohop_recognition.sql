-- The pairs of a scope of two paths (the grants a tenant has on a member and on the groups of the member) are written by a statement as the union of the two: the direct grants and the members
-- of the groups granted, as the sub-select of IN or EXISTS, through a view or an inlined function. The scan reads them from the scope units when the union is exactly the pair set:
-- two selects, each over the relation of its path with the predicate of the btree that defines its rows and the key binding and no other condition, both for the same key.
-- Every other union keeps the plan that was always exact, and says why. Each statement returns the rows of the
-- join as written (sift.agg_scope = off) and of the normal plan, after the build and after each change of the pairs.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET default_statistics_target = 1000;
-- page costs that keep a SiftAgg plan the cheaper one whenever it is generated: the plan then depends on the shape of the statement and the price of the two SiftAgg plans only
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE grants (id int PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int, deleted_at timestamptz, status text NOT NULL DEFAULT 'live') WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX grants_direct ON grants (dim_a_id, tenant_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX grants_group ON grants (group_id, tenant_id) WHERE deleted_at IS NULL;
CREATE TABLE memberships (id serial PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz, status text NOT NULL DEFAULT 'live') WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX memberships_active ON memberships (group_id, dim_a_id) WHERE deleted_at IS NULL;
CREATE INDEX grants_by_member ON grants USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX grants_by_group ON grants USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX memberships_by_member ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int, kind text) WITH (autovacuum_enabled = false);
CREATE TABLE my_tenants (tenant_id int PRIMARY KEY);
INSERT INTO my_tenants VALUES (2), (5);

-- member ids 1 to 58 are in the group 1 + m % 6, every fifth in a second group, every eleventh of them soft deleted from the first; members 59 and 60 are in none
INSERT INTO memberships (group_id, dim_a_id, deleted_at) SELECT 1 + m % 6, m, CASE WHEN m % 11 = 0 THEN now() END FROM generate_series(1, 58) m;
INSERT INTO memberships (group_id, dim_a_id) SELECT 1 + (m + 2) % 6, m FROM generate_series(5, 58, 5) m;
INSERT INTO grants (id, tenant_id, dim_a_id, group_id, deleted_at) VALUES
	(1, 1, 5, NULL, NULL),		-- tenant 1: member 5 directly, and through its two groups (6 and 2)
	(2, 1, 17, NULL, NULL),
	(3, 1, NULL, 6, NULL),
	(4, 1, NULL, 2, NULL),
	(5, 2, NULL, 3, NULL),		-- tenant 2: a group, and a soft deleted direct grant
	(6, 2, 8, NULL, now()),
	(7, 3, 1, NULL, NULL),		-- tenant 3: three member ids, and a soft deleted group
	(8, 3, 2, NULL, NULL),
	(9, 3, 3, NULL, NULL),
	(10, 3, NULL, 4, now()),
	(11, 4, NULL, 5, NULL),		-- tenant 4: a group, and a group nobody is in
	(12, 4, NULL, 7, NULL),
	(13, 5, 40, NULL, NULL),	-- tenant 5: a member and a group that has it
	(14, 5, NULL, 5, NULL),
	(15, 6, 50, 3, NULL);		-- tenant 6: one row that is a direct and a joined grant at once
INSERT INTO facts SELECT i, CASE WHEN i % 17 = 0 THEN NULL WHEN i % 19 = 0 THEN 100000 + i ELSE 1 + (i * 7) % 60 END, timestamptz '2026-01-01' + i * interval '9 minutes', i % 10, 'k' || i % 4
	FROM generate_series(1, 3000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, amount, kind)
	WITH (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)');
-- built before the journals existed: the stamps are the next build's
REINDEX INDEX facts_col;
VACUUM (FREEZE, ANALYZE) facts;
VACUUM (ANALYZE) grants, memberships, my_tenants;

-- the union as the pair set, in the views an application keeps and in the function it calls
CREATE VIEW tenant_pairs AS
	SELECT g.dim_a_id AS member, g.tenant_id FROM grants g WHERE g.deleted_at IS NULL
	UNION
	SELECT m.dim_a_id, g.tenant_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL;
CREATE VIEW tenant_pairs_all AS
	SELECT g.dim_a_id AS member, g.tenant_id FROM grants g WHERE g.deleted_at IS NULL
	UNION ALL
	SELECT m.dim_a_id, g.tenant_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL;
CREATE VIEW tenant_pairs_barrier WITH (security_barrier) AS
	SELECT g.dim_a_id AS member, g.tenant_id FROM grants g WHERE g.deleted_at IS NULL
	UNION
	SELECT m.dim_a_id, g.tenant_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL;
CREATE VIEW tenant_pairs_keyfirst AS
	SELECT g.tenant_id, g.dim_a_id AS member FROM grants g WHERE g.deleted_at IS NULL
	UNION
	SELECT g.tenant_id, m.dim_a_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL;
CREATE FUNCTION facts_of(t int) RETURNS SETOF facts LANGUAGE sql STABLE AS
$$ SELECT * FROM facts WHERE dim_a_id IN (SELECT g.dim_a_id FROM grants g WHERE g.tenant_id = t AND g.deleted_at IS NULL
	UNION SELECT m.dim_a_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.tenant_id = t AND g.deleted_at IS NULL AND m.deleted_at IS NULL) $$;

-- a member of tenant 1 reached by both paths (5), a soft deleted pair of the direct path (tenant 2, member 8), a tenant with no pair (9)
SELECT (SELECT count(*) FROM (SELECT member FROM tenant_pairs WHERE tenant_id = 1) p) AS members_of_1, (SELECT count(*) FROM tenant_pairs_all WHERE tenant_id = 1) AS rows_of_1_union_all,
	(SELECT count(*) FROM tenant_pairs WHERE tenant_id = 9) AS members_of_9;

-- the selects of the union: cols 'm' projects the member, 'mk' the member and the key; k the condition that binds the key, x the conditions on the tables of the path
CREATE FUNCTION direct_arm(cols text, k text DEFAULT '', x text DEFAULT '') RETURNS text LANGUAGE sql IMMUTABLE AS
$$ SELECT 'SELECT g.dim_a_id AS member' || CASE cols WHEN 'mk' THEN ', g.tenant_id' ELSE '' END || ' FROM grants g WHERE g.deleted_at IS NULL' || k || x $$;
CREATE FUNCTION joined_arm(cols text, k text DEFAULT '', xg text DEFAULT '', xm text DEFAULT '') RETURNS text LANGUAGE sql IMMUTABLE AS
$$ SELECT 'SELECT m.dim_a_id' || CASE cols WHEN 'mk' THEN ', g.tenant_id' ELSE '' END || ' FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL' || k || xg || xm $$;
CREATE FUNCTION in_stmt(u text) RETURNS text LANGUAGE sql IMMUTABLE AS
$$ SELECT 'SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE f.dim_a_id IN (' || u || ') GROUP BY kind' $$;

CREATE TABLE forms (name text PRIMARY KEY, q text);
INSERT INTO forms VALUES
 -- IN and EXISTS: a member counts once, whichever paths reach it and however many keys
 ('IN', in_stmt(direct_arm('m', ' AND g.tenant_id = 1') || ' UNION ' || joined_arm('m', ' AND g.tenant_id = 1'))),
 ('IN, UNION ALL', in_stmt(direct_arm('m', ' AND g.tenant_id = 1') || ' UNION ALL ' || joined_arm('m', ' AND g.tenant_id = 1'))),
 ('IN, arms in the other order', in_stmt(joined_arm('m', ' AND g.tenant_id = 1') || ' UNION ' || direct_arm('m', ' AND g.tenant_id = 1'))),
 ('IN, two keys', in_stmt(direct_arm('m', ' AND g.tenant_id IN (2, 5)') || ' UNION ' || joined_arm('m', ' AND g.tenant_id IN (2, 5)'))),
 ('IN, columns that are not NULL', in_stmt(direct_arm('m', ' AND g.tenant_id = 1', ' AND g.dim_a_id IS NOT NULL') || ' UNION ' ||
	joined_arm('m', ' AND g.tenant_id = 1', ' AND g.group_id IS NOT NULL', ' AND m.dim_a_id IS NOT NULL'))),
 ('IN, a tenant with no pair', in_stmt(direct_arm('m', ' AND g.tenant_id = 9') || ' UNION ' || joined_arm('m', ' AND g.tenant_id = 9'))),
 ('EXISTS', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE EXISTS (SELECT 1 FROM tenant_pairs v WHERE v.member = f.dim_a_id AND v.tenant_id = 1) GROUP BY kind$q$),
 ('IN, view', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE f.dim_a_id IN (SELECT member FROM tenant_pairs WHERE tenant_id = 1) GROUP BY kind$q$),
 ('IN, view, UNION ALL', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE f.dim_a_id IN (SELECT member FROM tenant_pairs_all WHERE tenant_id = 1) GROUP BY kind$q$),
 ('IN, view, two keys', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE f.dim_a_id IN (SELECT member FROM tenant_pairs WHERE tenant_id IN (2, 5)) GROUP BY kind$q$),
 ('IN, barrier view', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE f.dim_a_id IN (SELECT member FROM tenant_pairs_barrier WHERE tenant_id = 1) GROUP BY kind$q$),
 ('IN, view with the key first', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE f.dim_a_id IN (SELECT member FROM tenant_pairs_keyfirst WHERE tenant_id = 1) GROUP BY kind$q$),
 ('IN, table of keys', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE f.dim_a_id IN (SELECT member FROM tenant_pairs WHERE tenant_id IN (SELECT tenant_id FROM my_tenants)) GROUP BY kind$q$),
 ('function', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts_of(1) GROUP BY kind$q$),
 ('top 5, view', $q$SELECT f.dim_a_id, sum(amount) AS total FROM facts f WHERE f.dim_a_id IN (SELECT member FROM tenant_pairs WHERE tenant_id = 1) GROUP BY f.dim_a_id ORDER BY total DESC, f.dim_a_id LIMIT 5$q$),
 -- a union that is not the two paths of the scope
 ('IN, one path', in_stmt(direct_arm('m', ' AND g.tenant_id = 1'))),
 ('IN, one path, the joined one', in_stmt(joined_arm('m', ' AND g.tenant_id = 1'))),
 ('IN, a key in one arm only', in_stmt(direct_arm('m', ' AND g.tenant_id = 1') || ' UNION ' || joined_arm('m'))),
 ('IN, other keys in the arms', in_stmt(direct_arm('m', ' AND g.tenant_id = 1') || ' UNION ' || joined_arm('m', ' AND g.tenant_id = 2'))),
 ('IN, no key', in_stmt(direct_arm('m') || ' UNION ' || joined_arm('m'))),
 ('IN, a condition on the direct path', in_stmt(direct_arm('m', ' AND g.tenant_id = 1', ' AND g.status = ''live''') || ' UNION ' || joined_arm('m', ' AND g.tenant_id = 1'))),
 ('IN, a condition on the groups', in_stmt(direct_arm('m', ' AND g.tenant_id = 1') || ' UNION ' || joined_arm('m', ' AND g.tenant_id = 1', ' AND g.status = ''live'''))),
 ('IN, a condition on the memberships', in_stmt(direct_arm('m', ' AND g.tenant_id = 1') || ' UNION ' || joined_arm('m', ' AND g.tenant_id = 1', '', ' AND m.status = ''live'''))),
 ('IN, a condition that is no binding of the key', in_stmt(direct_arm('m', ' AND g.tenant_id < 2') || ' UNION ' || joined_arm('m', ' AND g.tenant_id < 2'))),
 ('IN, the rows that are not pairs', in_stmt('SELECT g.dim_a_id FROM grants g WHERE g.deleted_at IS NOT NULL AND g.tenant_id = 1 UNION ' || joined_arm('m', ' AND g.tenant_id = 1'))),
 ('IN, the direct path without its predicate', in_stmt('SELECT g.dim_a_id FROM grants g WHERE g.tenant_id = 1 UNION ' || joined_arm('m', ' AND g.tenant_id = 1'))),
 ('IN, the groups without their predicate', in_stmt(direct_arm('m', ' AND g.tenant_id = 1') ||
	' UNION SELECT m.dim_a_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE m.deleted_at IS NULL AND g.tenant_id = 1')),
 ('IN, the memberships without their predicate', in_stmt(direct_arm('m', ' AND g.tenant_id = 1') ||
	' UNION SELECT m.dim_a_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND g.tenant_id = 1')),
 ('IN, no join of the groups to the memberships', in_stmt(direct_arm('m', ' AND g.tenant_id = 1') ||
	' UNION SELECT m.dim_a_id FROM grants g, memberships m WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL AND g.tenant_id = 1')),
 ('IN, the groups joined by another column', in_stmt(direct_arm('m', ' AND g.tenant_id = 1') ||
	' UNION SELECT m.dim_a_id FROM grants g JOIN memberships m ON m.id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL AND g.tenant_id = 1')),
 ('IN, a third table in the joined path', in_stmt(direct_arm('m', ' AND g.tenant_id = 1') ||
	' UNION SELECT m.dim_a_id FROM grants g JOIN memberships m ON m.group_id = g.group_id JOIN my_tenants t ON t.tenant_id = g.tenant_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL AND g.tenant_id = 1')),
 ('IN, DISTINCT in an arm', in_stmt('SELECT DISTINCT g.dim_a_id FROM grants g WHERE g.deleted_at IS NULL AND g.tenant_id = 1 UNION ' || joined_arm('m', ' AND g.tenant_id = 1'))),
 ('IN, LIMIT in an arm', in_stmt('(' || direct_arm('m', ' AND g.tenant_id = 1') || ' LIMIT 100) UNION ' || joined_arm('m', ' AND g.tenant_id = 1'))),
 ('IN, an arm that aggregates', in_stmt('SELECT max(g.dim_a_id) FROM grants g WHERE g.deleted_at IS NULL AND g.tenant_id = 1 UNION ' || joined_arm('m', ' AND g.tenant_id = 1'))),
 ('IN, a column that is not the member', in_stmt('SELECT g.id FROM grants g WHERE g.deleted_at IS NULL AND g.tenant_id = 1 UNION SELECT m.id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL AND g.tenant_id = 1')),
 ('IN, three selects', in_stmt(direct_arm('m', ' AND g.tenant_id = 1') || ' UNION ' || joined_arm('m', ' AND g.tenant_id = 1') || ' UNION SELECT 7')),
 ('IN, INTERSECT', in_stmt(direct_arm('m', ' AND g.tenant_id = 1') || ' INTERSECT ' || joined_arm('m', ' AND g.tenant_id = 1'))),
 ('IN, EXCEPT', in_stmt(direct_arm('m', ' AND g.tenant_id = 1') || ' EXCEPT ' || joined_arm('m', ' AND g.tenant_id = 1'))),
 ('IN, a path and a table that is none', in_stmt(direct_arm('m', ' AND g.tenant_id = 1') || ' UNION SELECT t.tenant_id FROM my_tenants t')),
 ('IN, a path and a sub-select of keys', in_stmt(direct_arm('m', ' AND g.tenant_id = 1') || ' UNION ' || joined_arm('m', ' AND g.tenant_id IN (SELECT tenant_id FROM my_tenants)'))),
 ('IN, a union that reads no scope', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE f.dim_a_id IN (SELECT tenant_id FROM my_tenants UNION SELECT 7) GROUP BY kind$q$),
 ('IN, the member or the group of a grant', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE f.dim_a_id IN (SELECT coalesce(g.dim_a_id, m.dim_a_id) FROM grants g LEFT JOIN memberships m ON m.group_id = g.group_id AND m.deleted_at IS NULL WHERE g.tenant_id = 1 AND g.deleted_at IS NULL) GROUP BY kind$q$);

-- the plan: the scope read (keys, semantics), or the join and the reason the scope pass gave (sift.explain_why, arm scope), else the plan
CREATE FUNCTION label(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; keys text; r text; plan text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		plan := plan || l || E'\n';
	END LOOP;
	IF plan ~ 'Scope Index: ' THEN
		keys := substring(plan from 'Scope Keys: ([^\n]*)');
		RETURN 'scope read, ' || keys || ' keys, ' || substring(plan from 'Scope Semantics: ([^\n]*)');
	END IF;
	SELECT regexp_replace(reason, '^.*the join runs as written \((.*)\)$', '\1') INTO r FROM sift.explain_why(q) WHERE arm = 'scope';
	RETURN CASE WHEN r IS NOT NULL THEN 'join, ' || r WHEN plan ~ 'Lookup Joins' THEN 'join' WHEN plan ~ 'SiftAgg' THEN 'SiftAgg' ELSE 'normal plan' END;
END $$;

-- the rows of the statement as planned, with sift.agg_scope off and on the normal plan
CREATE FUNCTION rows_md5(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	DROP TABLE IF EXISTS scope_rows;
	EXECUTE 'CREATE TEMP TABLE scope_rows AS ' || q;
	SELECT coalesce(md5(string_agg(t::text, '|' ORDER BY t::text)), 'no rows') INTO r FROM scope_rows t;
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

SELECT name, label(q) AS plan, same(q) AS same_rows FROM forms ORDER BY name;

-- the key by a parameter: a generic plan reads it when the scan starts; a condition that reads a parameter is no proof that an arm takes the rows of its btree
PREPARE by_key(int) AS SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE f.dim_a_id IN (SELECT member FROM tenant_pairs WHERE tenant_id = $1) GROUP BY kind;
PREPARE by_keys(int[]) AS SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE f.dim_a_id IN (SELECT member FROM tenant_pairs WHERE tenant_id = ANY ($1)) GROUP BY kind;
PREPARE arms_by_key(int) AS SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE f.dim_a_id IN
	(SELECT g.dim_a_id FROM grants g WHERE g.tenant_id = $1 AND g.deleted_at IS NULL UNION SELECT m.dim_a_id FROM grants g JOIN memberships m ON m.group_id = g.group_id
		WHERE g.tenant_id = $1 AND g.deleted_at IS NULL AND m.deleted_at IS NULL) GROUP BY kind;
PREPARE arms_by_status(text) AS SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f WHERE f.dim_a_id IN
	(SELECT g.dim_a_id FROM grants g WHERE g.tenant_id = 1 AND g.deleted_at IS NULL AND g.status = $1 UNION SELECT m.dim_a_id FROM grants g JOIN memberships m ON m.group_id = g.group_id
		WHERE g.tenant_id = 1 AND g.deleted_at IS NULL AND m.deleted_at IS NULL) GROUP BY kind;
SET plan_cache_mode = force_generic_plan;
SELECT label('EXECUTE by_key(1)') AS by_key, label('EXECUTE by_keys(ARRAY[2, 5])') AS by_keys,
	label('EXECUTE arms_by_key(1)') AS arms_by_key, label('EXECUTE arms_by_status(''live'')') AS arms_by_status;
SELECT same('EXECUTE by_key(1)') AS by_key, same('EXECUTE by_keys(ARRAY[2, 5])') AS by_keys,
	same('EXECUTE arms_by_key(1)') AS arms_by_key, same('EXECUTE arms_by_status(''live'')') AS arms_by_status;
SET plan_cache_mode = force_custom_plan;
SELECT label('EXECUTE by_key(1)') AS by_key, label('EXECUTE by_keys(ARRAY[2, 5])') AS by_keys,
	label('EXECUTE arms_by_key(1)') AS arms_by_key, label('EXECUTE arms_by_status(''live'')') AS arms_by_status;
SELECT same('EXECUTE by_key(1)') AS by_key, same('EXECUTE by_keys(ARRAY[2, 5])') AS by_keys,
	same('EXECUTE arms_by_key(1)') AS arms_by_key, same('EXECUTE arms_by_status(''live'')') AS arms_by_status;
RESET plan_cache_mode;

-- the pairs change after the build: the reader patches the member ids whose pairs changed (each of the three journals says which) and the rows are the join's in every state
CREATE FUNCTION states(note text) RETURNS TABLE (state text, name text, plan text, same_rows bool) LANGUAGE sql AS $$
	SELECT note, f.name, label(f.q), same(f.q) FROM forms f
	WHERE f.name IN ('IN', 'IN, two keys', 'IN, view', 'IN, a tenant with no pair', 'function', 'top 5, view') ORDER BY f.name
$$;
SELECT * FROM states('built');
INSERT INTO grants (id, tenant_id, dim_a_id) VALUES (20, 1, 30), (21, 9, 31), (22, 9, 32);
SELECT * FROM states('direct grants inserted');
UPDATE grants SET deleted_at = now() WHERE id = 2;
SELECT * FROM states('a direct grant soft deleted');
UPDATE grants SET deleted_at = NULL WHERE id = 2;
SELECT * FROM states('a direct grant restored');
UPDATE grants SET group_id = 1 WHERE id = 3;
SELECT * FROM states('a group grant moved to another group');
INSERT INTO grants (id, tenant_id, group_id) VALUES (23, 9, 3);
SELECT * FROM states('a group granted');
UPDATE grants SET deleted_at = now() WHERE id = 23;
SELECT * FROM states('a group grant soft deleted');
INSERT INTO memberships (group_id, dim_a_id) VALUES (1, 59), (2, 60);
SELECT * FROM states('memberships added to groups');
UPDATE memberships SET deleted_at = now() WHERE group_id = 2 AND dim_a_id = 7;
SELECT * FROM states('a member soft deleted from a group');
UPDATE memberships SET deleted_at = NULL WHERE group_id = 2 AND dim_a_id = 7;
SELECT * FROM states('a member restored to a group');
DELETE FROM memberships WHERE group_id = 1 AND dim_a_id = 59;
SELECT * FROM states('a membership deleted');
UPDATE facts SET dim_a_id = 59 WHERE id <= 40;
SELECT * FROM states('facts moved to an added member, write buffer');
REINDEX INDEX facts_col;
SELECT * FROM states('rebuilt');

-- the privileges of the statement: the plan of the scope read has no scan of the tables of the arms, and the role still needs what the join needs, as the owner of a view it reads through
CREATE ROLE twohop_reader NOLOGIN;
CREATE FUNCTION denied(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'rows';
EXCEPTION WHEN insufficient_privilege THEN RETURN sqlerrm;
END $$;
CREATE TABLE roles_forms (name text PRIMARY KEY, q text);
INSERT INTO roles_forms SELECT name, q FROM forms WHERE name IN ('IN', 'IN, view', 'IN, one path');
GRANT SELECT ON facts, tenant_pairs, roles_forms TO twohop_reader;
SET ROLE twohop_reader;
SELECT name, denied(q) AS no_privilege_on_the_tables FROM roles_forms ORDER BY name;
RESET ROLE;
GRANT SELECT ON grants TO twohop_reader;
SET ROLE twohop_reader;
SELECT name, denied(q) AS no_privilege_on_memberships FROM roles_forms ORDER BY name;
RESET ROLE;
GRANT SELECT ON memberships TO twohop_reader;
SET ROLE twohop_reader;
SELECT name, denied(q) AS privileges FROM roles_forms ORDER BY name;
-- a plan made for the role that had the privilege: the role that lost it is refused
PREPARE kept_for_role AS SELECT count(*) FROM facts f WHERE f.dim_a_id IN (SELECT member FROM tenant_pairs WHERE tenant_id = 1);
PREPARE kept_for_role_inline AS SELECT count(*) FROM facts f WHERE f.dim_a_id IN (SELECT g.dim_a_id FROM grants g WHERE g.tenant_id = 1 AND g.deleted_at IS NULL
	UNION SELECT m.dim_a_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.tenant_id = 1 AND g.deleted_at IS NULL AND m.deleted_at IS NULL);
SELECT label('EXECUTE kept_for_role_inline') AS for_the_role, denied('EXECUTE kept_for_role_inline') AS allowed;
RESET ROLE;
REVOKE SELECT ON memberships FROM twohop_reader;
SET ROLE twohop_reader;
SELECT denied('EXECUTE kept_for_role_inline') AS revoked;
RESET ROLE;
-- row-level security on a table of an arm: the policy is a condition of its own, which the arm cannot be proved to leave out
GRANT SELECT ON memberships TO twohop_reader;
ALTER TABLE grants ENABLE ROW LEVEL SECURITY;
CREATE POLICY grants_all ON grants FOR SELECT USING (status = 'live');
SET ROLE twohop_reader;
SELECT name, label(q) AS plan, same(q) AS same_rows FROM roles_forms WHERE name IN ('IN', 'IN, view') ORDER BY name;
RESET ROLE;
DROP POLICY grants_all ON grants;
ALTER TABLE grants DISABLE ROW LEVEL SECURITY;
DEALLOCATE ALL;
DROP TABLE roles_forms;
DROP FUNCTION denied(text);
DROP OWNED BY twohop_reader;
DROP ROLE twohop_reader;

-- the plan holds the relations of the arms and the scope index: a prepared statement is planned again when the btree of a path is replaced, and with no scope left
PREPARE kept AS SELECT count(*) FROM facts f WHERE f.dim_a_id IN (SELECT member FROM tenant_pairs WHERE tenant_id = 1);
SELECT label('EXECUTE kept') AS recorded_btrees, same('EXECUTE kept') AS same_rows;
CREATE UNIQUE INDEX grants_group_live ON grants (group_id, tenant_id) WHERE deleted_at IS NULL AND status = 'live';
DROP INDEX facts_col;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, amount, kind)
	WITH (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) using grants_group_live via memberships(group_id, dim_a_id)');
REINDEX INDEX facts_col;
SELECT label('EXECUTE kept') AS stricter_btree, same('EXECUTE kept') AS same_rows;
DROP INDEX facts_col;
SELECT label('EXECUTE kept') AS no_scope_index, same('EXECUTE kept') AS same_rows;

DEALLOCATE ALL;
DROP FUNCTION label(text), rows_md5(text), same(text), states(text), facts_of(int), direct_arm(text, text, text), joined_arm(text, text, text, text), in_stmt(text);
DROP TABLE forms;
DROP VIEW tenant_pairs, tenant_pairs_all, tenant_pairs_barrier, tenant_pairs_keyfirst;
DROP TABLE facts, grants, memberships, my_tenants;
DROP EXTENSION pg_sift;
