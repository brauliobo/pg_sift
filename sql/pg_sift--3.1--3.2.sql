\echo Use "ALTER EXTENSION pg_sift UPDATE TO '3.2'" to load this file. \quit

-- Paths of a jsonb column of a sift_table (docs/COL-JSONB-31.md section 4): a table access method has no options, so the declaration is a
-- row here, by table and column name, in the grammar of the `paths` option of sift_col for that one column. A change applies to the segments
-- written afterwards (promotion, merge, VACUUM FULL); until every segment has the paths a query reads the column as before.
CREATE TABLE sift.table_paths (relid regclass NOT NULL, attname name NOT NULL, spec text NOT NULL, PRIMARY KEY (relid, attname));
SELECT pg_catalog.pg_extension_config_dump('sift.table_paths', '');

-- Checks that the table can hold the paths and rebuilds its layout (C).
CREATE FUNCTION sift.table_paths_check(tbl regclass, col name, spec text) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_table_paths_check' LANGUAGE C VOLATILE;

-- Declares the paths of column col of the sift_table tbl; an empty or NULL spec removes the declaration.
CREATE FUNCTION sift.table_set_paths(tbl regclass, col name, spec text) RETURNS void
	LANGUAGE plpgsql VOLATILE AS $fn$
BEGIN
	PERFORM sift.table_paths_check(tbl, col, spec);
	IF coalesce(spec, '') = '' THEN
		DELETE FROM sift.table_paths WHERE relid = tbl AND attname = col;
	ELSE
		INSERT INTO sift.table_paths VALUES (tbl, col, spec) ON CONFLICT (relid, attname) DO UPDATE SET spec = EXCLUDED.spec;
	END IF;
END
$fn$;

-- The declaration goes with the table.
CREATE FUNCTION sift.table_paths_drop() RETURNS event_trigger
	LANGUAGE plpgsql AS $fn$
BEGIN
	DELETE FROM sift.table_paths p USING pg_event_trigger_dropped_objects() d
		WHERE d.classid = 'pg_class'::regclass AND d.objsubid = 0 AND p.relid = d.objid;
END
$fn$;

CREATE EVENT TRIGGER sift_table_paths_drop ON sql_drop EXECUTE FUNCTION sift.table_paths_drop();

-- VACUUM's work on the column segments and the file of a sift_table, without its indexes (so without the tombstone rewrite, which needs them): the
-- dead rows are cleared, write buffers promoted, mostly dead segments rewritten, free space gathered and the free end of the file returned.
-- busy: the table, or the lock of its end, is in use and the call did nothing there. Background maintenance does the same (sift.table_auto_defrag).
CREATE FUNCTION sift.table_maintain(tbl regclass,
									OUT busy bool, OUT dead_rows int8, OUT promoted int4, OUT merged int4,
									OUT blocks_before int8, OUT blocks_after int8)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_table_maintain' LANGUAGE C STRICT VOLATILE;

-- The rewrite of sift.col_auto_merge, now and in this session (for a job: pg_cron): when the churn has scattered the index (many units with a dead row,
-- many units over the same superblocks, many rows in the write buffer), the write buffer is sealed and every segment merged into one, without
-- blocking readers or writers. False when nothing is due or the maintenance lock is held.
CREATE FUNCTION sift.col_maintain(index regclass) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_col_maintain' LANGUAGE C STRICT VOLATILE;

-- Why sift.col_maintain would rewrite the index now: 'dirty units', 'fragmented units' or 'write buffer rows' (the thresholds are sift.col_auto_merge_*),
-- NULL when it is not due. automatic: as the server's own rewrite, which leaves an index over sift.vacuum_rewrite_limit megabytes alone.
CREATE FUNCTION sift.col_maintenance_due(index regclass, automatic bool DEFAULT true) RETURNS text
	AS 'MODULE_PATHNAME', 'sift_col_maintenance_due' LANGUAGE C STRICT VOLATILE;
