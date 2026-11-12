-- The statements that join the scanned table to the UNION of the two paths of a scope and count a row for each pair: several keys, every key, the key as a group key, a key by a parameter.
-- The node holds the join over the union (a child plan of it) to run where the units cannot count a row for each pair or cost more. Every statement returns the rows of the join as
-- written (sift.agg_scope = off) and of the normal plan in every state of the pairs, with generic and custom plans, and with the held join run (sift.test_scope_join).
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

CREATE TABLE forms (name text PRIMARY KEY, q text);
INSERT INTO forms VALUES
 ('one key', $q$SELECT v.tenant_id, sum(amount) AS total, count(*) AS n FROM facts f JOIN tenant_pairs v ON v.member = f.dim_a_id WHERE v.tenant_id = 1 GROUP BY v.tenant_id$q$),
 ('two keys', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f JOIN tenant_pairs v ON v.member = f.dim_a_id WHERE v.tenant_id IN (2, 5) GROUP BY kind$q$),
 ('two keys, grouped by the key', $q$SELECT v.tenant_id, sum(amount) AS total, count(*) AS n FROM facts f JOIN tenant_pairs v ON v.member = f.dim_a_id WHERE v.tenant_id IN (2, 5) GROUP BY v.tenant_id$q$),
 ('every key', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM facts f JOIN tenant_pairs v ON v.member = f.dim_a_id GROUP BY kind$q$),
 ('every key, grouped by the key', $q$SELECT v.tenant_id, sum(amount) AS total, count(*) AS n FROM facts f JOIN tenant_pairs v ON v.member = f.dim_a_id GROUP BY v.tenant_id$q$),
 ('every key, grouped by the key and another column', $q$SELECT v.tenant_id, kind, sum(amount) AS total, count(*) AS n FROM facts f JOIN tenant_pairs v ON v.member = f.dim_a_id GROUP BY v.tenant_id, kind$q$),
 ('every key, count(DISTINCT)', $q$SELECT v.tenant_id, count(DISTINCT f.id) AS n FROM facts f JOIN tenant_pairs v ON v.member = f.dim_a_id GROUP BY v.tenant_id$q$),
 ('the union written in the join', $q$SELECT v.tenant_id, sum(amount) AS total, count(*) AS n FROM facts f JOIN (SELECT g.dim_a_id AS member, g.tenant_id FROM grants g WHERE g.deleted_at IS NULL UNION SELECT m.dim_a_id, g.tenant_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL) v ON v.member = f.dim_a_id GROUP BY v.tenant_id$q$),
 ('barrier view', $q$SELECT v.tenant_id, sum(amount) AS total, count(*) AS n FROM facts f JOIN tenant_pairs_barrier v ON v.member = f.dim_a_id GROUP BY v.tenant_id$q$),
 -- what stays a join, with its reason
 ('UNION ALL, grouped by the key', $q$SELECT v.tenant_id, sum(amount) AS total, count(*) AS n FROM facts f JOIN tenant_pairs_all v ON v.member = f.dim_a_id GROUP BY v.tenant_id$q$),
 ('a condition on the pairs', $q$SELECT v.tenant_id, sum(amount) AS total, count(*) AS n FROM facts f JOIN tenant_pairs v ON v.member = f.dim_a_id WHERE v.member > 10 GROUP BY v.tenant_id$q$),
 ('a left join', $q$SELECT v.tenant_id, sum(amount) AS total, count(*) AS n FROM facts f LEFT JOIN tenant_pairs v ON v.member = f.dim_a_id GROUP BY v.tenant_id$q$);

CREATE FUNCTION label(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; keys text; r text; plan text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		plan := plan || l || E'\n';
	END LOOP;
	IF plan ~ 'Scope Index: ' THEN
		keys := substring(plan from 'Scope Keys: ([^\n]*)');
		RETURN 'scope read, ' || keys || ' keys, ' || substring(plan from 'Scope Semantics: ([^\n]*)') || CASE WHEN plan ~ 'Scope Group Key' THEN ', key grouped' ELSE '' END;
	END IF;
	SELECT regexp_replace(reason, '^.*the join runs as written \((.*)\)$', '\1') INTO r FROM sift.explain_why(q) WHERE arm = 'scope';
	RETURN CASE WHEN r IS NOT NULL THEN 'join, ' || r WHEN plan ~ 'SiftAgg' THEN 'SiftAgg' ELSE 'normal plan' END;
END $$;
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
-- the held join run: the child plan of the union gives the join's rows
CREATE FUNCTION held(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'not run';
BEGIN
	PERFORM set_config('sift.test_scope_join', 'on', false);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Scope Fallback' THEN r := 'join run'; END IF;
	END LOOP;
	r := r || CASE WHEN same(q) THEN ', same rows' ELSE ', ROWS DIFFER' END;
	PERFORM set_config('sift.test_scope_join', 'off', false);
	RETURN r;
END $$;
CREATE FUNCTION states(note text) RETURNS TABLE (state text, name text, plan text, same_rows bool, held_join text) LANGUAGE sql AS $$
	SELECT note, f.name, label(f.q), same(f.q), CASE WHEN label(f.q) ~ '^scope read' THEN held(f.q) ELSE '-' END FROM forms f ORDER BY f.name
$$;

SELECT * FROM states('built');

-- the key by a parameter: custom and generic plans
PREPARE u_by_key(int) AS SELECT v.tenant_id, sum(amount) AS total, count(*) AS n FROM facts f JOIN tenant_pairs v ON v.member = f.dim_a_id WHERE v.tenant_id = $1 GROUP BY v.tenant_id;
PREPARE u_by_keys(int[]) AS SELECT v.tenant_id, sum(amount) AS total, count(*) AS n FROM facts f JOIN tenant_pairs v ON v.member = f.dim_a_id WHERE v.tenant_id = ANY ($1) GROUP BY v.tenant_id;
SET plan_cache_mode = force_generic_plan;
SELECT label('EXECUTE u_by_key(1)') AS by_key, label('EXECUTE u_by_keys(ARRAY[2, 5])') AS by_keys, same('EXECUTE u_by_key(1)') AS same_key, same('EXECUTE u_by_keys(ARRAY[2, 5])') AS same_keys;
SET plan_cache_mode = force_custom_plan;
SELECT label('EXECUTE u_by_key(1)') AS by_key, label('EXECUTE u_by_keys(ARRAY[2, 5])') AS by_keys, same('EXECUTE u_by_key(1)') AS same_key, same('EXECUTE u_by_keys(ARRAY[2, 5])') AS same_keys;
RESET plan_cache_mode;

-- the pairs change after the build: the reader patches or refuses, the rows are the join's in every state
INSERT INTO grants (id, tenant_id, dim_a_id) VALUES (20, 1, 30), (21, 9, 31), (22, 9, 32);
SELECT * FROM states('direct grants inserted');
UPDATE grants SET deleted_at = now() WHERE id = 2;
SELECT * FROM states('a direct grant soft deleted');
UPDATE grants SET group_id = 1 WHERE id = 3;
SELECT * FROM states('a group grant moved to another group');
INSERT INTO memberships (group_id, dim_a_id) VALUES (1, 59), (2, 60);
SELECT * FROM states('memberships added to groups');
UPDATE memberships SET deleted_at = now() WHERE group_id = 2 AND dim_a_id = 7;
SELECT * FROM states('a member soft deleted from a group');
UPDATE facts SET dim_a_id = 59 WHERE id <= 40;
SELECT * FROM states('facts moved to an added member, write buffer');
REINDEX INDEX facts_col;
SELECT * FROM states('rebuilt');

DEALLOCATE ALL;
DROP FUNCTION states(text), held(text), same(text), rows_md5(text), label(text);
DROP TABLE forms;
DROP VIEW tenant_pairs, tenant_pairs_all, tenant_pairs_barrier;
DROP TABLE facts, grants, memberships, my_tenants;
DROP EXTENSION pg_sift;
