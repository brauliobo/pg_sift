-- A pair arrives for a member whose rows the build counted with no key, rows of members with pairs are promoted after it, and the rows of the build are deleted but that one, so that a merge
-- rewrites the parts that held them: the read of the key takes the pair that changed since the summary, before and after the maintenance step, and is the join's.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
-- the scope summary is Format 2.0; the testing library builds new indexes in Format 1.0 when told to
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
\ir test/common/sql/col/scope/base.sql

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT m, 1 + m % 8 FROM generate_series(1, 100) m;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE FUNCTION reads(OUT same boolean, OUT route text) LANGUAGE sql AS $$
	SELECT r.same, r.route FROM scope_read('SELECT count(*), sum(v), min(id) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3') r $$;

-- the first row has a member with no pair, the others have one
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, CASE WHEN i = 1 THEN 301 ELSE 1 + i % 100 END, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10 FROM generate_series(1, 6000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT * FROM reads();
-- the pair of that member arrives, then rows of members that have pairs are promoted
INSERT INTO tenant_scope VALUES (301, 3);
INSERT INTO events SELECT 100000 + i, 1 + i % 100, timestamptz '2026-01-01', i % 10 FROM generate_series(1, 3000) i;
SELECT sift.col_promote('events_col') >= 0 AS promoted;
-- the rows of the build are deleted but the first, and the merge rewrites the parts that held them
DELETE FROM events WHERE id BETWEEN 2 AND 6000;
VACUUM events;
SELECT sift.col_merge('events_col', false) >= 0 AS merged;
SELECT * FROM reads();
SELECT sift.col_maintain('events_col') AS rewritten;
SELECT * FROM reads();
SELECT sift.col_verify('events_col', true);

DROP TABLE events, tenant_scope;
DROP FUNCTION reads();
DROP FUNCTION scope_run(text, text), scope_group_run(text), scope_reason_ok(text), scope_read(text);
DROP EXTENSION pg_sift;
