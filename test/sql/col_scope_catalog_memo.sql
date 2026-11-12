-- A read of a scope asks the catalogs for what only DDL changes (the triggers that make the journal of its pairs complete, the relations of the scope and their journal indexes, its legs, the rows of its
-- relations, the units of its keys) once for a state of the catalogs, not once for each read: the second read of a key is answered from the memos (sift._test_catalog_memos counts the answers), a
-- transaction that has an xid (its own DDL is not counted before it commits) asks every time, and a trigger of the journal that was disabled and enabled again, which any other backend learns of as
-- a message, ends the memo: the journal is no longer the whole truth, and the read says so and gives the join's rows. Counts only, no timings.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;
SET sift.test_scope_patch = on;

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT d, 1 + (d + j * 3) % 8 FROM generate_series(1, 64) d, generate_series(0, 1) j;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + (i * 7) % 64, timestamptz '2026-01-01' + i * interval '9 minutes', i % 10 FROM generate_series(1, 4000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
VACUUM (FREEZE, ANALYZE) events, tenant_scope;

-- one read of the key k: the rows against the join, whether the units were read
CREATE FUNCTION read_key(k int8, OUT stored boolean, OUT same boolean) LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	a text;
	b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT count(*), sum(amount) FROM events f WHERE sift.in_scope(''events_col''::regclass, ''tenant_id'', f.dim_a_id, ARRAY[%s]::int8[])', k) INTO p;
	EXECUTE format('SELECT json_agg(t)::text FROM (SELECT count(*), sum(amount) FROM events f WHERE sift.in_scope(''events_col''::regclass, ''tenant_id'', f.dim_a_id, ARRAY[%s]::int8[])) t', k) INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('SELECT json_agg(t)::text FROM (SELECT count(*), sum(amount) FROM events f WHERE f.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = %s)) t', k) INTO b;
	SET LOCAL sift.enable_agg = on;
	same := a = b;
	stored := jsonb_path_exists(p, '$.**."Scope Units Read"');
END $$;

-- the first read asks the catalogs, the second is answered by the memos
SELECT * FROM read_key(2);
SELECT sift._test_catalog_memos() AS h0 \gset
SELECT * FROM read_key(2);
SELECT sift._test_catalog_memos() > :h0 AS the_second_read_is_answered_by_the_memos;

-- a transaction with an xid asks every time
BEGIN;
INSERT INTO tenant_scope VALUES (100, 2);
SELECT sift._test_catalog_memos() AS h1 \gset
SELECT * FROM read_key(3);
SELECT sift._test_catalog_memos() = :h1 AS no_memo_in_a_transaction_with_an_xid;
ROLLBACK;

-- the triggers disabled while a pair is inserted and enabled again: the memo does not hide it
SELECT * FROM read_key(2);
ALTER TABLE tenant_scope DISABLE TRIGGER ALL;
INSERT INTO tenant_scope VALUES (101, 2);
DELETE FROM tenant_scope WHERE dim_a_id = 1 AND tenant_id = 2;
ALTER TABLE tenant_scope ENABLE TRIGGER ALL;
SELECT * FROM read_key(2);
SELECT * FROM read_key(4);

DROP FUNCTION read_key(int8);
DROP TABLE events, tenant_scope;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
