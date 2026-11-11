-- A scope read is exact whatever the maintenance did to the entity segments and the scope segments, and it is read from the scope units whenever they cover the rows (sift_scope_covered): the
-- scope segments are those of the generation of the entity segments they were written with, none of the entity segments is newer than the scope segments that stand for it.
-- (a) a promotion of new facts and then a VACUUM of the table (a promotion writes the scope segments of its rows, VACUUM rewrites the entity segments it cleans: the scope entries of the rows
--     it removed go with them), with the facts deleted and updated in between;
-- (b) a build with a small segment size writes many entity segments and as many scope segments, in whatever generation order: the reads are from the units, not refused as `do not cover the rows`.
-- The relation of the pairs has its journal index; the first scope index of a relation leaves its stamps unfilled until a REINDEX, which each section does after the build.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;
\ir test/scope_helpers.sql

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 12 FROM generate_series(1, 300) m, generate_series(0, 5) j WHERE j < m % 7 ON CONFLICT DO NOTHING;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');

-- the reads of the section: one key and a set of keys counted once, as the join counts them; every row says whether the units were read and whether the rows are the join's
CREATE FUNCTION reads(rel regclass, idx regclass, OUT n int, OUT same boolean, OUT stored boolean, OUT why text) RETURNS SETOF record LANGUAGE plpgsql AS $$
DECLARE
	k record;
BEGIN
	FOR k IN SELECT * FROM (VALUES (1, '1'), (2, '5'), (3, '12'), (4, 'ARRAY[2, 9]::int8[]'), (5, 'ARRAY[1, 3, 5, 7, 9, 11]::int8[]')) v(n, keys) LOOP
		SELECT r.same, r.stored, r.why INTO same, stored, why
		FROM scope_run(
			format('SELECT count(*), sum(v), min(id), max(id), count(DISTINCT dim_a_id) FROM %s e WHERE sift.in_scope(%L::regclass, ''tenant_id'', e.dim_a_id, %s)', rel, idx, k.keys),
			format('SELECT count(*), sum(v), min(id), max(id), count(DISTINCT dim_a_id) FROM %s e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ANY (%s::int8[]))', rel,
				CASE WHEN k.keys LIKE 'ARRAY%' THEN k.keys ELSE 'ARRAY[' || k.keys || ']::int8[]' END)) r;
		n := k.n;
		RETURN NEXT;
	END LOOP;
END $$;

-- (a)
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + (i * 7919) % 320, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 60) FROM generate_series(1, 6000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
REINDEX INDEX events_col;
VACUUM (FREEZE, ANALYZE) events;
SELECT * FROM reads('events', 'events_col');

INSERT INTO events SELECT i, 1 + i % 320, timestamptz '2026-06-01' + i * interval '1 minute', i % 10, 'y' FROM generate_series(6001, 8000) i;
SELECT sift.col_promote('events_col') >= 0 AS promoted;
SELECT * FROM reads('events', 'events_col');
DELETE FROM events WHERE id % 4 = 0;
UPDATE events SET v = v + 1 WHERE id % 9 = 0;
VACUUM events;
SELECT * FROM reads('events', 'events_col');
SELECT sift.col_verify('events_col', true);
SELECT sift.col_promote('events_col') >= 0 AS promoted_again;
VACUUM events;
SELECT * FROM reads('events', 'events_col');
SELECT sift.col_verify('events_col', true);
DROP TABLE events;

-- (b)
SET sift.test_col_segment_kb = 64;
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + (i * 7919) % 320, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 30000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
REINDEX INDEX events_col;
VACUUM (FREEZE, ANALYZE) events;
WITH sc AS (SELECT DISTINCT gen FROM sift._test_col_scope_entries('events_col')),
	seg AS (SELECT s.gen, s.gen IN (SELECT gen FROM sc) AS is_scope FROM sift.col_segments('events_col') s WHERE s.kind = 'immutable')
SELECT count(*) FILTER (WHERE NOT is_scope) > 1 AS entity_segments, count(*) FILTER (WHERE is_scope) > 1 AS scope_segments FROM seg;
SELECT * FROM reads('events', 'events_col');
SELECT sift.col_verify('events_col', true);

DROP TABLE events, tenant_scope;
DROP FUNCTION reads(regclass, regclass);
DROP FUNCTION scope_run(text, text), scope_group_run(text, text), scope_reason_ok(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
