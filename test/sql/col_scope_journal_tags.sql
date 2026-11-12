-- The journal of a scope relation tags each record with the scope key of its pair, so that a read of some keys is not charged for the pairs that changed in others: the stored entries of a dim_a_id
-- whose pair with another key changed are still right for the key read, and a read of that key lists no changed dim_a_id at all. Table-driven: each state of the pairs (they accumulate) read for
-- each key and for a list of keys: the rows are the join's, the changed dim_a_id values and the patched ones are those of the pairs that changed in the keys read. A journal made for another
-- column of the relation than a scope's key leaves that scope every change (it reads them all: the rows are the join's too). Counts only, no timings.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;
SET sift.test_scope_patch = on;

-- 8 keys; dim_a_id d has a pair with the keys 1 + d % 8 and 1 + (d + 3) % 8; 4,000 events
CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, region_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT d, 1 + (d + j * 3) % 8, 1 + (d + j) % 3 FROM generate_series(1, 64) d, generate_series(0, 1) j;
CREATE UNIQUE INDEX tenant_scope_by_region ON tenant_scope (dim_a_id, region_id);
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id, region_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + (i * 7) % 64, timestamptz '2026-01-01' + i * interval '9 minutes', i % 10 FROM generate_series(1, 4000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
CREATE TABLE events_region (LIKE events) WITH (autovacuum_enabled = false);
INSERT INTO events_region SELECT * FROM events;
CREATE INDEX events_region_col ON events_region USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = 'region_id: dim_a_id -> tenant_scope(dim_a_id, region_id)');
VACUUM (FREEZE, ANALYZE) events, events_region;
VACUUM (ANALYZE) tenant_scope;

-- one read of the keys (an int8 array) of the scope index idx: the rows against the join, whether the units were read, and the dim_a_id values the read listed as changed and patched
CREATE FUNCTION read_keys(idx regclass, scope text, keys int8[], OUT same boolean, OUT stored boolean, OUT changed int, OUT patched int) LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	a text;
	b text;
	tbl regclass := (SELECT indrelid FROM pg_index WHERE indexrelid = idx);
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT count(*), sum(amount) FROM %s f WHERE sift.in_scope(%L::regclass, %L, f.dim_a_id, %L::int8[])', tbl, idx, scope, keys) INTO p;
	EXECUTE format('SELECT json_agg(t)::text FROM (SELECT count(*), sum(amount) FROM events f WHERE sift.in_scope(%L::regclass, %L, f.dim_a_id, %L::int8[])) t', idx, scope, keys) INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('SELECT json_agg(t)::text FROM (SELECT count(*), sum(amount) FROM %s f WHERE f.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE %I = ANY (%L::int8[]))) t', tbl, scope, keys) INTO b;
	SET LOCAL sift.enable_agg = on;
	same := a = b;
	stored := jsonb_path_exists(p, '$.**."Scope Units Read"');
	changed := coalesce((jsonb_path_query_first(p, '$.**."Scope Changed Keys"') #>> '{}')::int, 0);
	patched := coalesce((jsonb_path_query_first(p, '$.**."Scope Patched Keys"') #>> '{}')::int, 0);
END $$;

-- the line of a state: the rows are the join's and the units are read for every read; the changed dim_a_id values of each key 1..8 read alone, of a list of two keys, and of the region scope's keys 1..4
CREATE FUNCTION state_line(OUT same boolean, OUT stored boolean, OUT changed int[], OUT list_changed int, OUT list_patched int, OUT region_changed int[]) LANGUAGE plpgsql AS $$
DECLARE
	l record;
	g record;
BEGIN
	SELECT bool_and(r.same), bool_and(r.stored), array_agg(r.changed ORDER BY k.k) INTO same, stored, changed
	FROM generate_series(1, 8) k(k), LATERAL read_keys('events_col', 'tenant_id', ARRAY[k.k]::int8[]) r;
	SELECT * INTO l FROM read_keys('events_col', 'tenant_id', ARRAY[2, 5]::int8[]);
	same := same AND l.same;
	stored := stored AND l.stored;
	list_changed := l.changed;
	list_patched := l.patched;
	SELECT bool_and(r.same) AS s, bool_and(r.stored) AS st, array_agg(r.changed ORDER BY k.k) AS ch INTO g FROM generate_series(1, 4) k(k), LATERAL read_keys('events_region_col', 'region_id', ARRAY[k.k]::int8[]) r;
	same := same AND g.s;
	stored := stored AND g.st;
	region_changed := g.ch;
END $$;

CREATE TABLE states (n int, label text, edit text);
INSERT INTO states VALUES
	(1, 'clean', 'SELECT 1'),
	(2, 'the pairs of key 1 of six dim_a_id values deleted and inserted again',
		'WITH d AS (DELETE FROM tenant_scope WHERE tenant_id = 1 AND dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1 ORDER BY dim_a_id LIMIT 6) RETURNING *) INSERT INTO tenant_scope SELECT * FROM d'),
	(3, 'a pair moved from key 2 to key 3', 'UPDATE tenant_scope SET tenant_id = 3 WHERE tenant_id = 2 AND dim_a_id = (SELECT min(dim_a_id) FROM tenant_scope WHERE tenant_id = 2) AND NOT EXISTS (SELECT 1 FROM tenant_scope t WHERE t.tenant_id = 3 AND t.dim_a_id = (SELECT min(dim_a_id) FROM tenant_scope WHERE tenant_id = 2))'),
	(4, 'a pair of key 5 deleted', 'DELETE FROM tenant_scope WHERE tenant_id = 5 AND dim_a_id = (SELECT min(dim_a_id) FROM tenant_scope WHERE tenant_id = 5)'),
	(5, 'pairs of key 8 inserted for dim_a_id values with events and none yet in it', 'INSERT INTO tenant_scope SELECT d, 8, 5 FROM generate_series(1, 64) d WHERE d % 8 <> 7 AND d % 8 <> 4 LIMIT 5 ON CONFLICT DO NOTHING'),
	(6, 'a pair of key 1 and region 2 gets region 4', 'UPDATE tenant_scope SET region_id = 4 WHERE dim_a_id = 40 AND tenant_id = 1');
SELECT c FROM states, LATERAL unnest(ARRAY[format('SELECT %s AS n, %L AS state', n, label), edit, 'SELECT * FROM state_line()']) WITH ORDINALITY u(c, o) ORDER BY n, o \gexec

-- the patched dim_a_id values of the keys 1 and 5 after the states: the key that changed patches, the ones that did not patch nothing
SELECT k, r.same, r.stored, r.changed, r.patched FROM generate_series(1, 8) k, LATERAL read_keys('events_col', 'tenant_id', ARRAY[k]::int8[]) r ORDER BY k;
SELECT sift.col_verify('events_col', true);

-- a refresh makes the stamps current: no key lists a changed dim_a_id
SELECT sift.col_maintain('events_col') AS rewritten;
SELECT * FROM state_line();
DROP TABLE states;
DROP FUNCTION state_line();
DROP FUNCTION read_keys(regclass, text, int8[]);
DROP TABLE events, events_region, tenant_scope;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
