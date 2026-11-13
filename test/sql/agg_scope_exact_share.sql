-- The price of a scope read starts from the share of its units the pre-aggregates answer, which needs the pages of a unit all-visible. A unit cut by time holds the rows of a slice of time, so the heap
-- pages of a slice: rows inserted since the last VACUUM make their own slice not exact and leave the share of the others, where a heap that has one page that is not all-visible used to price every unit
-- as read row by row (a read of a large key was priced at the join's cost, and ran the join, for the hours between two VACUUMs). A scope without a time cut holds any page in a unit, and answers only from a clean heap.
-- Counts of the share the planner takes, rounded where it is a share, never timings.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT d, 1 + d % 4 FROM generate_series(1, 200) d;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE timed (id int PRIMARY KEY, dim_a_id int, created_at date, amount int) WITH (autovacuum_enabled = false);
CREATE TABLE untimed (id int PRIMARY KEY, dim_a_id int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO timed SELECT i, 1 + (i * 7) % 200, date '2025-01-01' + i / 2000, i % 10 FROM generate_series(1, 600000) i;
INSERT INTO untimed SELECT i, 1 + (i * 7) % 200, i % 10 FROM generate_series(1, 600000) i;
CREATE INDEX timed_col ON timed USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
CREATE INDEX untimed_col ON untimed USING sift_col (id, dim_a_id, amount) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
VACUUM (FREEZE, ANALYZE) timed, untimed, tenant_scope;

-- every page all-visible: the units answer
SELECT sift._test_scope_exact('timed_col'::regclass) AS timed, sift._test_scope_exact('untimed_col'::regclass) AS untimed;

-- 1% of the rows arrive and nothing has vacuumed them: the pages of the slice of time they are in are not all-visible
INSERT INTO timed SELECT 1000000 + i, 1 + (i * 7) % 200, date '2026-06-01', 1 FROM generate_series(1, 6000) i;
INSERT INTO untimed SELECT 1000000 + i, 1 + (i * 7) % 200, 1 FROM generate_series(1, 6000) i;
SELECT sift._test_scope_exact('timed_col'::regclass) BETWEEN 0.9 AND 0.999 AS timed_most, sift._test_scope_exact('untimed_col'::regclass) AS untimed;

DROP TABLE timed, untimed, tenant_scope;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
