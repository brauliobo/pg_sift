-- The entries of an entity segment are exact as of the stamp it was written under, and the read takes the pairs that changed since the oldest stamp of the scope segments as changed. The scope
-- segment that held that stamp had no live entry left when the rows that had pairs were deleted (the row that stays has a member with no pair), so the merge dropped it and the oldest stamp was
-- the next one's, newer than a pair that arrived for that member: the row read as having no entry for it. The entity segment says it is covered no more, the join answers, and the
-- maintenance writes the scope segments again.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;
\ir test/scope_helpers.sql

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT m, 1 + m % 8 FROM generate_series(1, 100) m;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE FUNCTION reads(OUT same boolean, OUT stored boolean, OUT why text) LANGUAGE sql AS $$
	SELECT r.same, r.stored, r.why FROM scope_run(
		'SELECT count(*), sum(v), min(id) FROM events e WHERE sift.in_scope(''events_col''::regclass, ''tenant_id'', e.dim_a_id, 3)',
		'SELECT count(*), sum(v), min(id) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 3)') r $$;

-- the first row has a member with no pair, the others have one
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, CASE WHEN i = 1 THEN 301 ELSE 1 + i % 100 END, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10 FROM generate_series(1, 6000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT * FROM reads();
-- the pair of that member arrives, then rows of members that have pairs are promoted under a stamp newer than it
INSERT INTO tenant_scope VALUES (301, 3);
INSERT INTO events SELECT 100000 + i, 1 + i % 100, timestamptz '2026-01-01', i % 10 FROM generate_series(1, 3000) i;
SELECT sift.col_promote('events_col') >= 0 AS promoted;
-- the rows of the build are deleted but the first: its scope segment has no live entry and the merge drops it
DELETE FROM events WHERE id BETWEEN 2 AND 6000;
VACUUM events;
SELECT sift.col_merge('events_col', false) >= 0 AS merged;
SELECT count(*) AS scope_segments_left FROM sift._test_col_scope_entries('events_col');
SELECT * FROM reads();
SELECT sift.col_maintain('events_col') AS rewritten;
SELECT * FROM reads();
SELECT sift.col_verify('events_col', true);

DROP TABLE events, tenant_scope;
DROP FUNCTION reads();
DROP FUNCTION scope_run(text, text), scope_group_run(text), scope_reason_ok(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
