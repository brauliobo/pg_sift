-- A join to the scope relation of a scope index (JOIN, IN, EXISTS by the declared columns, the scope key bound by constants) is planned a second time with the join taken out of the
-- FROM list and its conditions the scope conjunct of the scan (EXPLAIN: Scope Index), the same through a view, a security barrier view over the relation and an inlined SQL function;
-- the prices choose between the two plans. Every statement returns the rows of the statement with sift.agg_scope off and of the normal plan, after the build and after each change of the
-- pairs. Each near miss keeps the join and names its reason (sift.explain_why, arm scope); the reader's own refusals are the Scope Fallback of EXPLAIN ANALYZE.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET default_statistics_target = 1000;
-- page costs that keep a SiftAgg plan the cheaper one whenever it is generated: the plan then depends on the shape of the statement and the price of the two SiftAgg plans only
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at date, kind text, amount int) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + (i::bigint * 7919) % 400, date '2025-01-01' + i % 300, 'k' || i % 4, i % 100 FROM generate_series(1, 6000) i;
-- entities 1 to 360 are members of tenant 1 + id % 10, one in seven of a second tenant; the others are in no tenant
CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, valid_until date, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT i, 1 + i % 10 FROM generate_series(1, 360) i;
INSERT INTO tenant_scope SELECT i, 1 + (i * 3 + 1) % 10 FROM generate_series(1, 360) i WHERE i % 7 = 0 ON CONFLICT DO NOTHING;
CREATE TABLE my_tenants (tenant_id int PRIMARY KEY);
INSERT INTO my_tenants VALUES (2), (5), (9);
CREATE INDEX tenant_pairs_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, amount) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
-- the first scope index starts the journal of the pairs at its build: a rebuild stamps the scope segments
REINDEX INDEX events_col;
VACUUM (FREEZE, ANALYZE) events, tenant_scope, my_tenants;

CREATE VIEW scope_view AS SELECT dim_a_id, tenant_id FROM tenant_scope;
CREATE VIEW scope_barrier WITH (security_barrier) AS SELECT dim_a_id, tenant_id FROM tenant_scope;
CREATE VIEW scope_barrier_live WITH (security_barrier) AS SELECT dim_a_id, tenant_id FROM tenant_scope WHERE valid_until IS NULL;
CREATE VIEW scope_barrier_distinct WITH (security_barrier) AS SELECT DISTINCT dim_a_id, tenant_id FROM tenant_scope;
CREATE VIEW events_barrier WITH (security_barrier) AS SELECT * FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 3);
CREATE FUNCTION events_of_in(t int) RETURNS SETOF events LANGUAGE sql STABLE AS
$$ SELECT * FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = t) $$;
CREATE FUNCTION events_of_join(t int) RETURNS SETOF events LANGUAGE sql STABLE AS
$$ SELECT e.* FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = t $$;
CREATE FUNCTION events_of_volatile(t int) RETURNS SETOF events LANGUAGE sql VOLATILE AS
$$ SELECT * FROM events WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = t) $$;
CREATE FUNCTION scope_ids(t int) RETURNS SETOF int LANGUAGE sql STABLE AS $$ SELECT dim_a_id FROM tenant_scope WHERE tenant_id = t $$;

CREATE TABLE forms (name text PRIMARY KEY, q text);
INSERT INTO forms VALUES
 ('join', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3 GROUP BY kind$q$),
 ('join, the key first', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM tenant_scope s JOIN events e ON e.dim_a_id = s.dim_a_id WHERE 3 = s.tenant_id GROUP BY kind$q$),
 ('join, bigint key', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3::bigint GROUP BY kind$q$),
 ('join, one key in a list', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id IN (3, 3) GROUP BY kind$q$),
 ('join, other conditions of the facts', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3 AND e.created_at >= date '2025-03-01' AND e.amount > 10 GROUP BY kind$q$),
 ('join, distinct count', $q$SELECT kind, count(DISTINCT e.dim_a_id) AS ids FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3 GROUP BY kind$q$),
 ('IN', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 3) GROUP BY kind$q$),
 ('IN, two keys', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id IN (2, 5)) GROUP BY kind$q$),
 ('IN, three keys', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id IN (2, 5, 9)) GROUP BY kind$q$),
 ('IN, ANY array', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ANY (ARRAY[2, 5, 9])) GROUP BY kind$q$),
 ('IN, every key', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)) GROUP BY kind$q$),
 ('EXISTS', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = e.dim_a_id AND s.tenant_id = 3) GROUP BY kind$q$),
 ('EXISTS, two keys', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = e.dim_a_id AND s.tenant_id IN (2, 5)) GROUP BY kind$q$),
 ('CTE', $q$WITH ids AS (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 3) SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM ids) GROUP BY kind$q$),
 ('plain view', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e JOIN scope_view s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3 GROUP BY kind$q$),
 ('barrier view, join', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e JOIN scope_barrier s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3 GROUP BY kind$q$),
 ('barrier view, IN', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM scope_barrier WHERE tenant_id = 3) GROUP BY kind$q$),
 ('function, IN', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events_of_in(3) GROUP BY kind$q$),
 ('function, join', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events_of_join(3) GROUP BY kind$q$),
 ('function of ids', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE e.dim_a_id IN (SELECT * FROM scope_ids(3)) GROUP BY kind$q$),
 ('through another join', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e JOIN my_tenants k ON k.tenant_id = 5 WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 5) GROUP BY kind$q$),
 ('top 5 ids', $q$SELECT e.dim_a_id, sum(amount) AS total FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 3) GROUP BY e.dim_a_id ORDER BY total DESC, e.dim_a_id LIMIT 5$q$),
 ('latest 5', $q$SELECT id, created_at FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 3) ORDER BY created_at DESC, id LIMIT 5$q$),
 -- near misses: each keeps the join
 ('miss: two keys, a row for each pair', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id IN (2, 5, 9) GROUP BY kind$q$),
 ('miss: condition on a pair column', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 3 AND valid_until IS NULL) GROUP BY kind$q$),
 ('miss: key not bound', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope) GROUP BY kind$q$),
 ('miss: group by the key', $q$SELECT s.tenant_id, sum(amount) AS total, count(*) AS n FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id IN (2, 5, 9) GROUP BY s.tenant_id$q$),
 ('miss: range on the key', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id < 3) GROUP BY kind$q$),
 ('miss: another column read', $q$SELECT kind, s.valid_until, sum(amount) AS total FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3 GROUP BY kind, s.valid_until$q$),
 ('miss: wrong fact column', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE e.id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 3) GROUP BY kind$q$),
 ('miss: NOT EXISTS', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE NOT EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = e.dim_a_id AND s.tenant_id = 3) GROUP BY kind$q$),
 ('miss: view with a condition', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e JOIN scope_barrier_live s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3 GROUP BY kind$q$),
 ('miss: view with DISTINCT', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e JOIN scope_barrier_distinct s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3 GROUP BY kind$q$),
 ('not a scope: volatile function', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events_of_volatile(3) GROUP BY kind$q$),
 ('not a scope: barrier view over the table', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events_barrier GROUP BY kind$q$);

-- the plan: the scope read (keys, and the semantics of several: one key reads the same rows once or per pair, and PostgreSQL 19 plans an IN of one key as a join), or the join and the reason the scope pass gave (sift.explain_why, arm scope), else the plan
CREATE FUNCTION label(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; keys text; r text; plan text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		plan := plan || l || E'\n';
	END LOOP;
	IF plan ~ 'Scope Index: ' THEN
		keys := substring(plan from 'Scope Keys: (\d+)');
		RETURN 'scope read, ' || keys || ' keys' || CASE WHEN keys = '1' THEN '' ELSE ', ' || substring(plan from 'Scope Semantics: ([^\n]*)') END;
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
	SELECT md5(string_agg(t::text, '|' ORDER BY t::text)) INTO r FROM scope_rows t;
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

-- what the run did: the reader's refusal, or the keys it patched
CREATE FUNCTION run_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Scope (Fallback|Changed Keys|Patched Keys|Dedupe Keys|Members Listed)' THEN r := r || regexp_replace(l, '^ *Scope ', '') || '; '; END IF;
	END LOOP;
	RETURN CASE WHEN r = '' THEN 'units' ELSE rtrim(r, '; ') END;
END $$;

SELECT name, label(q) AS plan, same(q) AS same_rows FROM forms ORDER BY name;

-- the switch: with sift.agg_scope off nothing is taken out
SET sift.agg_scope = off;
SELECT name, label(q) FROM forms WHERE name IN ('join', 'IN', 'IN, three keys') ORDER BY name;
RESET sift.agg_scope;

-- several keys: a member in two of them counts once for IN and EXISTS, which the reader does; the join counts it for each pair and keeps the join. The rows are the join's
SELECT count(*) > 0 AS members_in_two_keys FROM (SELECT dim_a_id FROM tenant_scope WHERE tenant_id IN (2, 5, 9) GROUP BY dim_a_id HAVING count(*) > 1) t;
SELECT name, run_of(q) AS run FROM forms WHERE name IN ('IN, two keys', 'IN, three keys', 'IN, ANY array', 'EXISTS, two keys') ORDER BY name;

-- a parameter of a generic plan is a key the scan evaluates when it starts (agg_scope_param_keys); the custom plan has the constant
PREPARE by_param(int) AS SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = $1) GROUP BY kind;
SET plan_cache_mode = force_custom_plan;
SELECT label('EXECUTE by_param(3)') AS custom_plan, same('EXECUTE by_param(3)') AS same_rows;
RESET plan_cache_mode;
DEALLOCATE by_param;

-- the pairs change after the build: the reader patches the members that changed (the journal says which), from every form
CREATE FUNCTION states(note text) RETURNS TABLE (state text, name text, run text, same_rows bool) LANGUAGE sql AS $$
	SELECT note, f.name, run_of(f.q), same(f.q) FROM forms f WHERE f.name IN ('join', 'IN', 'IN, three keys', 'EXISTS, two keys', 'barrier view, IN', 'top 5 ids', 'latest 5') ORDER BY f.name
$$;
DELETE FROM tenant_scope WHERE dim_a_id = 10 AND tenant_id = 1;
SELECT * FROM states('an ended pair');
INSERT INTO tenant_scope VALUES (370, 3, NULL), (371, 2, NULL), (371, 5, NULL);
SELECT * FROM states('inserted pairs');
UPDATE events SET dim_a_id = 371 WHERE id <= 40;
SELECT * FROM states('facts of a member of two keys moved, write buffer');
DELETE FROM tenant_scope WHERE dim_a_id = 20;
SELECT * FROM states('every pair of a member ended');
-- more changed members than the price of the patch allows (the bound of the entity rows of the table): the entity rows of the members the pairs list
SET sift.test_scope_patch = off;
SELECT * FROM states('more changed pairs than the bound');
RESET sift.test_scope_patch;
-- the changed pairs reach the reader as a verdict on a rewrite of the units
REINDEX INDEX events_col;
SELECT * FROM states('rebuilt');

DROP FUNCTION label(text), rows_md5(text), same(text), run_of(text), states(text);
DROP TABLE forms;
DROP VIEW scope_view, scope_barrier, scope_barrier_live, scope_barrier_distinct, events_barrier;
DROP FUNCTION events_of_in(int), events_of_join(int), events_of_volatile(int), scope_ids(int);
DROP TABLE events, tenant_scope, my_tenants;
DROP EXTENSION pg_sift;
