-- DDL that changes what the policies of a sift_table name: the TTL and the projections follow a renamed column, the TTL keeps its NOT NULL column, a projection keeps its columns, and the scope
-- and connect options of other tables follow a renamed or moved pair table. Every check prints t.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE tt (id int, at timestamptz NOT NULL, b int, c text) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO tt SELECT i, timestamptz '2030-01-01' + i * interval '1 minute', i % 100, 'c' || i FROM generate_series(1, 3000) i;
VACUUM tt;
SELECT sift.set_ttl('tt', 'at', '30 days');
SELECT sift.create_projection('tt', 'by_b', 'ORDER BY b INCLUDE (c)');
SELECT sift.create_projection('tt', 'sum_b', 'GROUP BY b MEASURES count(*)');

-- the column of the TTL is the one that keeps the rows of an expired part together: it stays NOT NULL and stays
ALTER TABLE tt ALTER COLUMN at DROP NOT NULL;
ALTER TABLE tt DROP COLUMN at;
SELECT count(*) = 1 AS ttl_column_untouched FROM sift.ttl_policies WHERE relid = 'tt'::regclass AND col = 'at';
SELECT attnotnull AS still_not_null FROM pg_attribute WHERE attrelid = 'tt'::regclass AND attname = 'at';

-- a column a projection names stays as long as the projection does
ALTER TABLE tt DROP COLUMN c;
ALTER TABLE tt DROP COLUMN b;
SELECT count(*) = 3 AS columns_kept FROM pg_attribute WHERE attrelid = 'tt'::regclass AND attname IN ('at', 'b', 'c');

-- RENAME COLUMN: the TTL and the definitions of the projections say the new name, the projections are built again
ALTER TABLE tt RENAME COLUMN at TO created;
ALTER TABLE tt RENAME COLUMN b TO bucket;
ALTER TABLE tt RENAME COLUMN c TO label;
SELECT relid::regclass::text, col FROM sift.ttl_policies WHERE relid = 'tt'::regclass;
SELECT name, spec FROM sift.projection_defs WHERE rel::oid = 'tt'::regclass::oid ORDER BY name;
VACUUM tt;
SELECT name, state FROM sift.projection_list() WHERE relation = 'tt'::regclass ORDER BY name;
SELECT parts_dropped = 0 AS ttl_runs_on_the_renamed_column FROM sift.drop_parts_before('tt', timestamptz '2020-01-01');

-- RENAME TABLE and SET SCHEMA: the policies are by OID and follow, and so do the options of the tables that name the pair table
CREATE TABLE pairs (member_id int NOT NULL, scope_key int NOT NULL, PRIMARY KEY (member_id, scope_key));
CREATE INDEX pairs_journal ON pairs USING sift_col (member_id, scope_key) WITH (track_changes = 'ctid');
CREATE TABLE facts (id int, member_id int, ts timestamptz NOT NULL, b int NOT NULL) USING sift_table WITH (autovacuum_enabled = false);
SELECT sift.table_set_option('facts', 'scope', 'tenant: member_id -> pairs(member_id, scope_key)');
ALTER TABLE pairs RENAME TO pairs_v2;
SELECT sift.table_get_option('facts', 'scope') AS scope_after_rename;
CREATE SCHEMA archive;
ALTER TABLE pairs_v2 SET SCHEMA archive;
SELECT sift.table_get_option('facts', 'scope') AS scope_after_set_schema;
ALTER TABLE facts RENAME COLUMN member_id TO who;
SELECT sift.table_get_option('facts', 'scope') AS scope_after_member_rename;

DROP TABLE tt, facts, archive.pairs_v2;
DROP SCHEMA archive;
DROP EXTENSION pg_sift;
