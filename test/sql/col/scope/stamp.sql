-- The stamp of a scope index (the journal position its entries and its rows with no entry are exact as of) does not depend on a scope segment existing: the index keeps it as a floor in its metapage,
-- set by the build and the first promotion, lowered by a promotion, raised by the refill that rewrites every scope segment, and left alone by VACUUM and by a merge that drops segments.
-- (a) a table whose pair relation is empty at the build (facts before grants): there is no scope segment, and the reads are from the units at once, exact, before and after the pairs arrive;
-- (b) the first scope segment of an index with many entity segments does not uncover them: the reads are from the units at once, and the members granted later are patched, after every step;
-- (c) the scope segments VACUUM drains lose no stamp: the rows that stay, whose members are granted later, are read exactly.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;
\ir test/scope_helpers.sql

-- one key and a set of keys counted once, as the join counts them: every row says whether the rows are the join's, whether the units were read and the scope was planned
CREATE FUNCTION reads(OUT n int, OUT same boolean, OUT stored boolean, OUT engaged boolean, OUT why text) RETURNS SETOF record LANGUAGE plpgsql AS $$
DECLARE
	k record;
BEGIN
	FOR k IN SELECT * FROM (VALUES (1, '3'), (2, 'ARRAY[2, 5, 7]::int8[]')) v(n, keys) LOOP
		SELECT r.same, r.stored, r.engaged, r.why INTO same, stored, engaged, why
		FROM scope_run(
			format('SELECT count(*), sum(v), min(id), max(id) FROM events e WHERE sift.in_scope(''events_col''::regclass, ''tenant_id'', e.dim_a_id, %s)', k.keys),
			format('SELECT count(*), sum(v), min(id), max(id) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ANY (%s::int8[]))',
				CASE WHEN k.keys LIKE 'ARRAY%' THEN k.keys ELSE 'ARRAY[' || k.keys || ']::int8[]' END)) r;
		n := k.n;
		RETURN NEXT;
	END LOOP;
END $$;

-- (a) the pair relation is empty
CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + i % 100, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10 FROM generate_series(1, 5000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT count(*) AS scope_segments FROM sift._test_col_scope_entries('events_col');
SELECT count(*) AS floors_set FROM sift._test_col_scope_floor('events_col') WHERE lineage <> 0;
SELECT * FROM reads();
INSERT INTO tenant_scope SELECT m, 1 + m % 8 FROM generate_series(1, 100) m;
SELECT * FROM reads();
INSERT INTO events SELECT 10000 + i, 1 + i % 100, timestamptz '2026-02-01', i % 10 FROM generate_series(1, 3000) i;
SELECT sift.col_promote('events_col') >= 0 AS promoted;
SELECT count(*) > 0 AS has_scope_segments FROM sift._test_col_scope_entries('events_col');
SELECT * FROM reads();
SELECT sift.col_maintain('events_col') AS rewritten;
SELECT * FROM reads();
SELECT sift.col_verify('events_col', true);
DROP TABLE events, tenant_scope;

-- (b) many entity segments whose members have no pair, then the first scope segment
SET sift.test_col_segment_kb = 64;
CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT m, 1 + m % 8 FROM generate_series(1, 20) m;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 101 + (i * 7919) % 200, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 30000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT count(*) > 1 AS entity_segments FROM sift.col_segments('events_col') WHERE kind = 'immutable';
SELECT count(*) AS scope_segments FROM sift._test_col_scope_entries('events_col');
SELECT * FROM reads();
-- the members of the old rows are granted: the old segments have no entries for them, the pairs are patched
INSERT INTO tenant_scope SELECT m, 3 FROM generate_series(101, 140) m;
SELECT * FROM reads();
-- the first rows whose members have pairs: the first scope segment comes under a stamp newer than those pairs
INSERT INTO events SELECT 100000 + i, 1 + i % 20, timestamptz '2026-03-01', i % 10, 'y' FROM generate_series(1, 3000) i;
SELECT sift.col_promote('events_col') >= 0 AS promoted;
SELECT count(*) > 0 AS has_scope_segments FROM sift._test_col_scope_entries('events_col');
SELECT * FROM reads();
-- more grants, a removed pair and a moved one, then more rows
INSERT INTO tenant_scope SELECT m, 5 FROM generate_series(141, 160) m;
DELETE FROM tenant_scope WHERE dim_a_id IN (105, 110) AND tenant_id = 3;
UPDATE tenant_scope SET tenant_id = 7 WHERE dim_a_id = 3;
SELECT * FROM reads();
INSERT INTO events SELECT 200000 + i, 1 + i % 160, timestamptz '2026-04-01', i % 10, 'z' FROM generate_series(1, 3000) i;
SELECT sift.col_promote('events_col') >= 0 AS promoted;
SELECT * FROM reads();
SELECT sift.col_maintain('events_col') AS rewritten;
SELECT * FROM reads();
SELECT sift.col_verify('events_col', true);
RESET sift.test_col_segment_kb;
DROP TABLE events, tenant_scope;

-- (c) the scope segments of the rows that had pairs are drained by VACUUM, the rows that stay get pairs later
CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT m, 1 + m % 8 FROM generate_series(1, 100) m;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, CASE WHEN i % 2 = 0 THEN 1 + i % 100 ELSE 301 + i % 100 END, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10 FROM generate_series(1, 6000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT * FROM reads();
CREATE TEMP TABLE floor_before AS SELECT * FROM sift._test_col_scope_floor('events_col');
DELETE FROM events WHERE dim_a_id <= 100;
VACUUM events;
SELECT count(*) AS scope_segments_left FROM sift._test_col_scope_entries('events_col');
SELECT count(*) = 0 AS floor_kept FROM (SELECT * FROM floor_before EXCEPT SELECT * FROM sift._test_col_scope_floor('events_col')) f;
SELECT * FROM reads();
INSERT INTO tenant_scope SELECT m, 3 FROM generate_series(301, 400) m;
SELECT * FROM reads();
INSERT INTO events SELECT 100000 + i, 1 + i % 100, timestamptz '2026-01-01', i % 10 FROM generate_series(1, 3000) i;
SELECT sift.col_promote('events_col') >= 0 AS promoted;
SELECT * FROM reads();
SELECT sift.col_maintain('events_col') AS rewritten;
SELECT * FROM reads();
SELECT sift.col_verify('events_col', true);

DROP TABLE events, tenant_scope, floor_before;
DROP FUNCTION reads();
DROP FUNCTION scope_run(text, text), scope_group_run(text), scope_reason_ok(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
