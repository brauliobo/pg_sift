-- A security barrier view is the statement's own to evaluate, in its own order: the scope node, which reads the summary of a scope or the scan of the facts, answers neither a statement over a
-- barrier view of the pairs nor one over a barrier view of the facts, and answers the same statements over the views without the barrier. Each line is a statement: whether its rows are those of
-- the join as written (sift.enable_agg = off) and what the plan read.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

CREATE TABLE events (id int NOT NULL PRIMARY KEY, dim_a_id int NOT NULL, created_at date NOT NULL, region text NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false);
CREATE TABLE grants (id serial PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE TABLE memberships (id serial PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX grants_direct ON grants (dim_a_id, tenant_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX grants_group ON grants (group_id, tenant_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX memberships_active ON memberships (group_id, dim_a_id) WHERE deleted_at IS NULL;
CREATE VIEW tenant_pairs AS
	SELECT g.dim_a_id AS member, g.tenant_id FROM grants g WHERE g.deleted_at IS NULL AND g.dim_a_id IS NOT NULL
	UNION
	SELECT m.dim_a_id, g.tenant_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL;
CREATE VIEW tenant_pairs_barrier WITH (security_barrier) AS
	SELECT g.dim_a_id AS member, g.tenant_id FROM grants g WHERE g.deleted_at IS NULL AND g.dim_a_id IS NOT NULL
	UNION
	SELECT m.dim_a_id, g.tenant_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL;
CREATE VIEW events_open AS SELECT * FROM events WHERE region <> 'JP';
CREATE VIEW events_barrier WITH (security_barrier) AS SELECT * FROM events WHERE region <> 'JP';

INSERT INTO grants (tenant_id, dim_a_id) SELECT 1, m FROM generate_series(1, 40) m;
INSERT INTO grants (tenant_id, group_id) VALUES (1, 11), (2, 21);
INSERT INTO memberships (group_id, dim_a_id) SELECT 11, m FROM generate_series(41, 70) m;
INSERT INTO memberships (group_id, dim_a_id) SELECT 21, m FROM generate_series(71, 90) m;
INSERT INTO events SELECT i, 1 + (i * 7) % 100, DATE '2022-01-01' + (i * 13) % 1400, (ARRAY['US', 'DE', 'BR', 'JP'])[1 + i % 4], i % 500 FROM generate_series(1, 10000) i;
CREATE INDEX grants_by_member ON grants USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX grants_by_group ON grants USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX memberships_by_member ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, region, amount)
	WITH (key_order = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)');
VACUUM (ANALYZE) events, grants, memberships;

CREATE FUNCTION probe(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; l text; plan text := '';
BEGIN
	SET LOCAL sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l; END LOOP;
	EXECUTE 'SELECT md5(coalesce(string_agg(r::text, ''|'' ORDER BY r::text), '''')) FROM (' || q || ') r' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT md5(coalesce(string_agg(r::text, ''|'' ORDER BY r::text), '''')) FROM (' || q || ') r' INTO b;
	RETURN (CASE WHEN a IS DISTINCT FROM b THEN 'DIFFERENT' ELSE 'same' END) || ' ' || (CASE WHEN plan LIKE '%SiftAggV6Scope%' THEN 'scope' WHEN plan LIKE '%SiftAggV6%' THEN 'aggregate scan' ELSE 'normal' END);
END $$;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

SELECT view, form, probe(replace(replace(q, 'PAIRS', pairs), 'FACTS', facts)) FROM (VALUES
	('aggregate', 'SELECT date_trunc(''month'', created_at) m, count(*), sum(amount) FROM FACTS WHERE dim_a_id IN (SELECT member FROM PAIRS WHERE tenant_id = 1) GROUP BY 1'),
	('join', 'SELECT count(*), sum(amount) FROM FACTS e JOIN PAIRS p ON p.member = e.dim_a_id WHERE p.tenant_id = 1'),
	('top rows', 'SELECT id, created_at FROM FACTS WHERE dim_a_id IN (SELECT member FROM PAIRS WHERE tenant_id = 1) ORDER BY created_at DESC, id DESC LIMIT 20')) f (form, q),
	(VALUES ('no barrier', 'events', 'tenant_pairs'), ('barrier on the pairs', 'events', 'tenant_pairs_barrier'), ('barrier on the facts', 'events_barrier', 'tenant_pairs'),
		('no barrier, a view of the facts', 'events_open', 'tenant_pairs')) v (view, facts, pairs)
	ORDER BY 1, 2;

DROP FUNCTION probe(text);
DROP VIEW tenant_pairs, tenant_pairs_barrier, events_open, events_barrier;
DROP TABLE events, grants, memberships;
DROP EXTENSION pg_sift;
