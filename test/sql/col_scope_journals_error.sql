-- An ERROR raised while the journals of a scope are being opened (a relation dropped or changed between the catalog lookup and the lock, a declaration that no longer holds) leaves the
-- state of the journals consistent: it counts the journals opened whole, so that whoever closes it closes exactly those. sift._test_scope_journals_open raises the error before the k-th open
-- of a relation or journal index (two for each journal) in a subtransaction and closes the state as a caller does; counted is what the state said at that point. A scope of one path has two
-- opens, one of two paths six; the opens go through the catalogs in a transaction with an xid and through the memo of them in a statement without one.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE grants (id int PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX grants_direct ON grants (dim_a_id, tenant_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX grants_group ON grants (group_id, tenant_id) WHERE deleted_at IS NULL;
CREATE TABLE memberships (id serial PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX memberships_active ON memberships (group_id, dim_a_id) WHERE deleted_at IS NULL;
CREATE TABLE pairs_one (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int) WITH (autovacuum_enabled = false);
INSERT INTO memberships (group_id, dim_a_id) SELECT 1 + (m - 1) / 10, m FROM generate_series(1, 60) m;
INSERT INTO grants VALUES (1, 1, NULL, 1, NULL), (2, 1, 25, NULL, NULL), (3, 2, NULL, 2, NULL);
INSERT INTO pairs_one SELECT d, 1 + d % 4 FROM generate_series(1, 60) d;
INSERT INTO facts SELECT i, 1 + (i * 7) % 60, timestamptz '2026-01-01' + i * interval '9 minutes', i % 10 FROM generate_series(1, 300) i;
CREATE INDEX grants_by_member ON grants USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX grants_by_group ON grants USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX memberships_by_member ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE INDEX pairs_one_journal ON pairs_one USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX facts_two ON facts USING sift_col (id, dim_a_id, created_at, amount)
	WITH (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)');
CREATE INDEX facts_one ON facts USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = 'tenant_id: dim_a_id -> pairs_one(dim_a_id, tenant_id)');
VACUUM (FREEZE) facts, grants, memberships, pairs_one;

-- the catalogs: a transaction with an xid asks them for every open
BEGIN;
SELECT pg_current_xact_id() IS NOT NULL AS has_xid;
SELECT k, sift._test_scope_journals_open('facts_two'::regclass, 0, k) FROM generate_series(1, 7) k;
SELECT k, sift._test_scope_journals_open('facts_one'::regclass, 0, k) FROM generate_series(1, 3) k;
COMMIT;

-- the memo of the catalogs: the first open without an error makes it, the errors go through it
SELECT sift._test_scope_journals_open('facts_two'::regclass, 0, 0) AS two_paths_opened;
SELECT sift._test_scope_journals_open('facts_one'::regclass, 0, 0) AS one_path_opened;
SELECT sift._test_catalog_memos() AS h0 \gset
SELECT k, sift._test_scope_journals_open('facts_two'::regclass, 0, k) FROM generate_series(1, 7) k;
SELECT k, sift._test_scope_journals_open('facts_one'::regclass, 0, k) FROM generate_series(1, 3) k;
SELECT sift._test_catalog_memos() - :h0 AS opens_answered_by_the_memo;

-- what an error left is gone: the scopes read and open as before
SELECT sift._test_scope_journals_open('facts_two'::regclass, 0, 0) AS two_paths_opened_again;
SELECT count(*) AS stamps FROM sift._test_scope_stamps('facts_two'::regclass, 0);

DROP TABLE facts, grants, memberships, pairs_one;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
