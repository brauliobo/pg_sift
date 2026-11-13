-- key_cut_rows, point_columns and order_columns, the options that 4.3 deprecated, are ignored since 5.0. They stay accepted and are ignored, so that an index that stores
-- one, a dump that names one and an ALTER INDEX of another option of such an index keep working: the layout is the one of an index without the option, a NOTICE says
-- so where it is written, and RESET drops it.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
\set VERBOSITY terse

-- no string column: the later indexes would share its strings through a value store and the first would not, which is a layout of its own
CREATE TABLE ro (id int, tenant_id int, uid int, ts timestamp) WITH (autovacuum_enabled = false);
INSERT INTO ro SELECT i, (i::bigint * 7919 % 300)::int, (i::bigint * 7919 % 40009)::int, timestamp '2026-01-01' + i * interval '1 second'
FROM generate_series(1, 40000) i;
VACUUM (FREEZE, ANALYZE) ro;

CREATE TABLE ro_opts (opt text, val text);
INSERT INTO ro_opts VALUES ('key_cut_rows', '64'), ('point_columns', '''uid'''), ('order_columns', '''ts''');

CREATE FUNCTION ro_layout(ix regclass) RETURNS text LANGUAGE sql AS $$
	SELECT sum(units) || ' units, ' || sum(rows) || ' rows, minor ' || min(format) || '-' || max(format) || ', ' || pg_relation_size(ix) || ' bytes'
	FROM sift.col_segments(ix) WHERE kind = 'immutable' $$;

-- an index with the option and the same index without it: the NOTICE of the option, nothing else is said
CREATE INDEX ro_plain ON ro USING sift_col (id, tenant_id, uid, ts) WITH (key_cut = 'tenant_id', point_tid_columns = 'off', order_array_columns = 'off');
SELECT format('CREATE INDEX ro_%1$s ON ro USING sift_col (id, tenant_id, uid, ts) WITH (key_cut = ''tenant_id'', point_tid_columns = ''off'', order_array_columns = ''off'', %1$s = %2$s)', opt, val)
FROM ro_opts \gexec
SELECT opt, ro_layout('ro_' || opt) = ro_layout('ro_plain') AS same_layout, (SELECT reloptions FROM pg_class WHERE relname = 'ro_' || opt) AS stored FROM ro_opts ORDER BY opt;

-- an index that stores one is read, takes the ALTER of another option and a REINDEX, and is verified, as one without it
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT opt, (SELECT count(*) || '/' || min(id) FROM ro WHERE uid = 7919) AS rows_of_a_key FROM ro_opts ORDER BY opt;
RESET enable_seqscan; RESET enable_bitmapscan;
SELECT format('ALTER INDEX ro_%s SET (cube_minmax = true)', opt) FROM ro_opts UNION ALL SELECT 'ALTER INDEX ro_plain SET (cube_minmax = true)' \gexec
SELECT format('REINDEX INDEX ro_%s', opt) FROM ro_opts \gexec
SELECT opt, ro_layout('ro_' || opt) = ro_layout('ro_plain') AS same_layout_after_reindex, sift.col_verify(('ro_' || opt)::regclass, true) IS NOT NULL AS verified FROM ro_opts ORDER BY opt;

-- written by ALTER INDEX: said again, and stored
SELECT format('ALTER INDEX ro_plain SET (%s = %s)', opt, val) FROM ro_opts \gexec
SELECT reloptions FROM pg_class WHERE relname = 'ro_plain';

-- sift.upgrade_commands() lists the RESET of an index that holds one, and no REINDEX for a point_columns that writes nothing
SELECT object::text, kind, command, required, reason ~ '^deprecated and ignored' AS says_ignored FROM sift.upgrade_commands() WHERE object::text ~ '^ro_' ORDER BY 1;

-- the dump of such an index names the options in its WITH list as stored, and CREATE INDEX accepts it
SELECT pg_get_indexdef('ro_plain'::regclass) AS dumped \gset
DROP INDEX ro_plain;
:dumped;
SELECT ro_layout('ro_plain') = ro_layout('ro_key_cut_rows') AS same_layout_as_dumped;

-- RESET drops them
SELECT format('ALTER INDEX ro_plain RESET (%s)', opt) FROM ro_opts \gexec
SELECT format('ALTER INDEX ro_%1$s RESET (%1$s)', opt) FROM ro_opts \gexec
SELECT relname, reloptions FROM pg_class WHERE relname = 'ro_plain' OR relname IN (SELECT 'ro_' || opt FROM ro_opts) ORDER BY relname;

DROP TABLE ro, ro_opts;
DROP FUNCTION ro_layout(regclass);
DROP EXTENSION pg_sift CASCADE;
