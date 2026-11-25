-- A statement that holds the scope relation of an index of a table it scans and is not read from the scope units says why in sift.explain_why, arm scope, whichever way it came to
-- be planned as written: it returns rows without an aggregate, a window, a scan that the planner does not make (a column or a condition the index does not hold, a row lock, a UNION ALL
-- of scope reads), a left join from the scanned table, sift.agg_scope or sift.enable_agg off. The reasons that name the site (the join is not by the declared column, a condition of its own)
-- are the ones of agg_scope_refusals; here every statement has one.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, kind text, amount int, other int) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + (i::bigint * 7919) % 400, 'k' || i % 4, i % 100, i % 7 FROM generate_series(1, 6000) i;
CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT i, 1 + i % 10 FROM generate_series(1, 360) i;
CREATE INDEX tenant_pairs_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, kind, amount) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
REINDEX INDEX events_col;
VACUUM (FREEZE, ANALYZE) events, tenant_scope;

CREATE FUNCTION label(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text; scoped bool := false;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		scoped := scoped OR l ~ 'Scope Index: ';
	END LOOP;
	SELECT regexp_replace(string_agg(reason, ' | '), '^.*the join runs as written \((.*)\)$', '\1') INTO r FROM sift.explain_why(q) WHERE arm = 'scope';
	RETURN CASE WHEN scoped THEN 'scope read' || coalesce(', and ' || r, '') ELSE 'join, ' || coalesce(r, 'no reason') END;
END $$;

CREATE TABLE forms (name text PRIMARY KEY, q text);
INSERT INTO forms VALUES
 ('rows of a join', $q$SELECT e.id FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3$q$),
 ('rows of an IN', $q$SELECT e.id FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 3)$q$),
 ('window over an IN', $q$SELECT e.id, sum(e.amount) OVER (PARTITION BY e.kind) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 3)$q$),
 ('column the index does not hold', $q$SELECT sum(e.other) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3$q$),
 ('condition on a column the index does not hold', $q$SELECT sum(e.amount) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3 AND e.other = 2$q$),
 ('left join from the facts', $q$SELECT sum(e.amount) FROM events e LEFT JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id AND s.tenant_id = 3$q$),
 ('UNION ALL of two IN', $q$SELECT sum(amount) FROM (SELECT amount FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 3) UNION ALL SELECT amount FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 4)) q$q$),
 ('row lock', $q$SELECT e.id FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 3) FOR UPDATE$q$),
 ('the pairs alone', $q$SELECT count(*) FROM tenant_scope WHERE tenant_id = 3$q$),
 ('the form the scope reads', $q$SELECT kind, sum(amount) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3 GROUP BY kind$q$);
SELECT name, label(q) AS plan FROM forms ORDER BY name;

SET sift.agg_scope = off;
SELECT label(q) AS agg_scope_off FROM forms WHERE name = 'the form the scope reads';
RESET sift.agg_scope;
SET sift.enable_agg = off;
SELECT label(q) AS enable_agg_off FROM forms WHERE name = 'the form the scope reads';
RESET sift.enable_agg;

-- a statement that is not a SELECT says what to pass
SELECT sift.explain_why($q$INSERT INTO events SELECT id + 100000, dim_a_id, kind, amount, other FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 3)$q$);

DROP FUNCTION label(text);
DROP TABLE forms;
DROP TABLE events, tenant_scope;
DROP EXTENSION pg_sift;
