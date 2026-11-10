-- key_cut_rows, point_columns and order_columns, the options that 4.3 deprecated, are accepted and ignored: the build decides by itself and says so once in its NOTICE
-- (and sift.index_choices), and an index that carries one keeps reading. sift.test_col_legacy_options (testing only) makes a build honor point_columns and order_columns until 4.4 removes both.
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

-- key_cut_rows (removed in 4.4) is accepted and ignored: the same units, a NOTICE where it is written, and an index that stores it still takes other options, a REINDEX and a RESET
CREATE INDEX dp_rows ON dp USING sift_col (id, tenant_id, pad) WITH (key_cut = 'tenant_id', key_cut_rows = 64);
CREATE INDEX dp_plain ON dp USING sift_col (id, tenant_id, pad) WITH (key_cut = 'tenant_id');
SELECT dp_units('dp_rows') = dp_units('dp_plain') AS same_units_as_without;
SELECT reloptions FROM pg_class WHERE relname = 'dp_rows';
ALTER INDEX dp_rows SET (cube_minmax = true);
REINDEX INDEX dp_rows;
SELECT dp_units('dp_rows') = dp_units('dp_plain') AS reindex_same_units;
ALTER INDEX dp_plain SET (key_cut_rows = 16);
ALTER INDEX dp_rows RESET (key_cut_rows);
SELECT reloptions FROM pg_class WHERE relname IN ('dp_rows', 'dp_plain') ORDER BY relname;
DROP INDEX dp_rows, dp_plain;

-- point_columns (removed in 4.4) is accepted and ignored: it lists nothing, where point_tid_columns lists the heap addresses that an equality reads with no unit read
CREATE TABLE dq (id int, uid int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO dq SELECT i, (i::bigint * 7919 % 40009)::int, md5(i::text) FROM generate_series(1, 40000) i;
VACUUM (FREEZE, ANALYZE) dq;
CREATE INDEX dq_ignored ON dq USING sift_col (id, uid, pad) WITH (point_columns = 'uid', point_tid_columns = 'off', key_cut = 'off');
CREATE INDEX dq_none ON dq USING sift_col (id, uid, pad) WITH (point_tid_columns = 'off', key_cut = 'off');
CREATE INDEX dq_tid ON dq USING sift_col (id, uid, pad) WITH (point_tid_columns = 'uid', key_cut = 'off');
SELECT i AS idx, c.units > 0 AS reads_units, c.rows_scanned
FROM unnest(ARRAY['dq_ignored', 'dq_none', 'dq_tid']) i, LATERAL sift._test_col_counters(i::regclass, '1:eq:7919', 24) c ORDER BY 1;
SELECT pg_relation_size('dq_ignored') = pg_relation_size('dq_none') AS same_size_as_without;
ALTER INDEX dq_ignored SET (cube_minmax = true);
REINDEX INDEX dq_ignored;
SELECT pg_relation_size('dq_ignored') = pg_relation_size('dq_none') AS reindex_same_size;
SELECT sift.col_verify('dq_ignored');
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*), min(id) FROM dq WHERE uid = 7919;
RESET enable_seqscan; RESET enable_bitmapscan;
ALTER INDEX dq_ignored RESET (point_columns);
SELECT reloptions FROM pg_class WHERE relname = 'dq_ignored';
DROP TABLE dq;

-- order_columns: no sorted run is written; the order arrays answer the top-N, and an index that has runs keeps reading them
CREATE TABLE dr (id int, ts timestamp, pad text) WITH (autovacuum_enabled = false);
INSERT INTO dr SELECT i, timestamp '2026-01-01' + (i::bigint * 7919 % 40009) * interval '1 minute', md5(i::text) FROM generate_series(1, 40000) i;
VACUUM (FREEZE, ANALYZE) dr;
CREATE FUNCTION dr_runs(ix regclass) RETURNS bigint LANGUAGE sql AS $$ SELECT count(*) FROM sift.col_sections(ix) WHERE section = 'runs' $$;
CREATE FUNCTION dr_top(n int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	SET LOCAL sift.enable_agg = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off;
	SELECT string_agg(id::text, ',') INTO a FROM (SELECT id FROM dr ORDER BY ts DESC, id LIMIT n) x;
	SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on;
	SELECT string_agg(id::text, ',') INTO b FROM (SELECT id FROM dr ORDER BY ts DESC, id LIMIT n) x;
	RETURN CASE WHEN a = b THEN 'same' ELSE 'different' END;
END $$;
SET sift.test_col_legacy_options = on;
CREATE INDEX dr_runs ON dr USING sift_col (id, ts, pad) WITH (order_columns = 'ts', key_cut = 'off');
SELECT dr_runs('dr_runs') > 0 AS legacy_writes_runs;
SELECT dp_choice('dr_runs', 'order_columns') IS NULL AS legacy_says_nothing;
SET sift.test_col_legacy_options = off;
CREATE INDEX dr_off ON dr USING sift_col (id, ts, pad) WITH (order_columns = 'ts', key_cut = 'off');
CREATE INDEX dr_arrays ON dr USING sift_col (id, ts, pad) WITH (order_columns = 'ts', order_array_columns = 'ts', key_cut = 'off');
CREATE INDEX dr_list ON dr USING sift_col (id, ts, pad) WITH (order_array_columns = 'ts', key_cut = 'off');
SELECT i AS idx, dr_runs(i::regclass) AS runs, (SELECT count(*) FROM sift.col_sections(i::regclass) WHERE section LIKE 'order%' OR section LIKE 'gorder%') > 0 AS has_array
FROM unnest(ARRAY['dr_off', 'dr_arrays', 'dr_list']) i ORDER BY 1;
SELECT dp_choice('dr_off', 'order_columns');
SELECT dp_choice('dr_list', 'order_columns') IS NULL AS no_advice_without;
-- the index built with runs answers as before while the option is ignored, and its next build writes none
SELECT dr_top(20), sift.col_verify('dr_runs');
REINDEX INDEX dr_runs;
SELECT dr_runs('dr_runs') AS runs_after_reindex, dr_top(20), sift.col_verify('dr_runs');
DROP TABLE dr;
DROP FUNCTION dr_runs(regclass), dr_top(int);

DROP TABLE dp CASCADE;
DROP FUNCTION dp_units(regclass), dp_choice(regclass, text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
