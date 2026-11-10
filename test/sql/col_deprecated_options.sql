-- The options that 4.3 deprecated are accepted and ignored: the build decides by itself, says so once in its NOTICE (and sift.index_choices), and an index that carries
-- one keeps reading. sift.test_col_legacy_options (testing only) makes a build honor them, which is how the tests of their layouts run until 4.4 removes both.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
\set VERBOSITY terse

CREATE TABLE dp (id int, tenant_id int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO dp SELECT i, (i::bigint * 7919 % 300)::int, md5(i::text) FROM generate_series(1, 40000) i;
VACUUM (FREEZE, ANALYZE) dp;

CREATE FUNCTION dp_units(ix regclass) RETURNS bigint LANGUAGE sql AS $$ SELECT sum(units)::bigint FROM sift.col_segments(ix) $$;
CREATE FUNCTION dp_choice(ix regclass, name text) RETURNS text LANGUAGE sql AS $$
	SELECT kind || ': ' || value FROM sift.index_choices(ix) WHERE choice = name $$;

-- key_cut_rows: the units aim at the rows of an average superblock, whatever the option says
SET sift.test_col_legacy_options = off;
CREATE INDEX dp_rows ON dp USING sift_col (id, tenant_id, pad) WITH (key_cut = 'tenant_id', key_cut_rows = 64);
CREATE INDEX dp_plain ON dp USING sift_col (id, tenant_id, pad) WITH (key_cut = 'tenant_id');
SELECT dp_units('dp_rows') = dp_units('dp_plain') AS same_units_as_without;
SELECT dp_choice('dp_rows', 'key_cut_rows');
SELECT dp_choice('dp_plain', 'key_cut_rows') IS NULL AS no_advice_without;
SET sift.test_col_legacy_options = on;
CREATE INDEX dp_legacy ON dp USING sift_col (id, tenant_id, pad) WITH (key_cut = 'tenant_id', key_cut_rows = 64);
SELECT dp_units('dp_legacy') > 10 * dp_units('dp_plain') AS legacy_cuts_small_units;
SELECT dp_choice('dp_legacy', 'key_cut_rows') IS NULL AS legacy_says_nothing;

-- an index built with the option under the legacy setting keeps its option and reads the same after a REINDEX that ignores it
SET sift.test_col_legacy_options = off;
SELECT reloptions FROM pg_class WHERE relname = 'dp_legacy';
SELECT count(*), sum(id) FROM dp WHERE tenant_id = 42;
REINDEX INDEX dp_legacy;
SELECT dp_units('dp_legacy') = dp_units('dp_plain') AS reindex_ignores_it;
SELECT dp_choice('dp_legacy', 'key_cut_rows');

-- point_columns: its leaves are listed as heap addresses, which an equality reads with no unit read, where the rows form reads the unit of each row
CREATE TABLE dq (id int, uid int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO dq SELECT i, (i::bigint * 7919 % 40009)::int, md5(i::text) FROM generate_series(1, 40000) i;
VACUUM (FREEZE, ANALYZE) dq;
SET sift.test_col_legacy_options = on;
CREATE INDEX dq_rows ON dq USING sift_col (id, uid, pad) WITH (point_columns = 'uid', key_cut = 'off');
SELECT dp_choice('dq_rows', 'point_columns') IS NULL AS legacy_says_nothing;
SET sift.test_col_legacy_options = off;
CREATE INDEX dq_addr ON dq USING sift_col (id, uid, pad) WITH (point_columns = 'uid', key_cut = 'off');
CREATE INDEX dq_tid ON dq USING sift_col (id, uid, pad) WITH (point_tid_columns = 'uid', key_cut = 'off');
SELECT i AS idx, c.units > 0 AS reads_units, c.rows_scanned
FROM unnest(ARRAY['dq_rows', 'dq_addr', 'dq_tid']) i, LATERAL sift._test_col_counters(i::regclass, '1:eq:7919', 24) c ORDER BY 1;
SELECT pg_relation_size('dq_addr') = pg_relation_size('dq_tid') AS same_size_as_point_tid;
SELECT dp_choice('dq_addr', 'point_columns');
SELECT dp_choice('dq_tid', 'point_columns') IS NULL AS no_advice_without;
SELECT sift.col_verify('dq_addr');
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*), min(id) FROM dq WHERE uid = 7919;
RESET enable_seqscan; RESET enable_bitmapscan;
DROP TABLE dq;

DROP TABLE dp CASCADE;
DROP FUNCTION dp_units(regclass), dp_choice(regclass, text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
