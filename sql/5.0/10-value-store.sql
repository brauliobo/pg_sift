
-- The shared value store of a column: one relation for each (table, column) that several sift_col indexes of the table share (docs/COL-VSTORE.md)
CREATE FUNCTION sift.values_handler(internal) RETURNS table_am_handler
	AS 'MODULE_PATHNAME', 'sift_values_handler' LANGUAGE C STRICT;
CREATE ACCESS METHOD sift_values TYPE TABLE HANDLER sift.values_handler;

CREATE FUNCTION sift.col_value_store_usage(store regclass, OUT epoch int8, OUT member regclass, OUT segments int4, OUT codes int8, OUT total_codes int8)
RETURNS SETOF record LANGUAGE c STABLE AS 'MODULE_PATHNAME', 'sift_col_value_store_usage';
COMMENT ON FUNCTION sift.col_value_store_usage(regclass) IS 'The codes of a shared value store that the segments of its indexes refer to: for each index and epoch its segments and the distinct codes they name, and for each epoch a row of member NULL with the codes any index names (the live ones); total_codes is what the store holds. Reads the dictionaries of every segment of the indexes that use the store.';

CREATE FUNCTION sift._col_value_store_begin(tbl regclass, col name) RETURNS regclass
	AS 'MODULE_PATHNAME', 'sift_col_value_store_begin' LANGUAGE C STRICT;
CREATE FUNCTION sift._col_value_store_end(store regclass) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_col_value_store_end' LANGUAGE C STRICT;

CREATE FUNCTION sift._col_value_store_drop_unused() RETURNS void
	AS 'MODULE_PATHNAME', 'sift_col_value_store_drop_unused' LANGUAGE C STRICT;
-- DROP INDEX of the last index that uses a store loads no library: this drops the store after any DROP that took an index
-- the role that drops the index has no right on the sift schema: the trigger runs as the owner of the extension
CREATE FUNCTION sift._col_value_store_sql_drop() RETURNS event_trigger LANGUAGE plpgsql
	SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
BEGIN
	IF EXISTS (SELECT 1 FROM pg_event_trigger_dropped_objects() WHERE object_type = 'index') THEN
		PERFORM sift._col_value_store_drop_unused();
	END IF;
END $$;
CREATE EVENT TRIGGER sift_col_value_store_sql_drop ON sql_drop EXECUTE FUNCTION sift._col_value_store_sql_drop();

CREATE FUNCTION sift.col_value_store_compact(tbl regclass, col name) RETURNS int8 LANGUAGE plpgsql AS $$
DECLARE
	st regclass := sift._col_value_store_begin(tbl, col);
	m regclass;
BEGIN
	IF st IS NULL THEN
		RETURN 0;
	END IF;
	FOR m IN SELECT DISTINCT u.member FROM sift.col_value_store_usage(st) u
	          WHERE u.member IS NOT NULL AND u.epoch < (SELECT max(v.epoch) FROM sift.col_value_store_usage(st) v)
	LOOP
		PERFORM sift.col_maintain(m);
	END LOOP;
	RETURN sift._col_value_store_end(st);
END $$;
COMMENT ON FUNCTION sift.col_value_store_compact(regclass, name) IS 'Frees the garbage of the shared value store of a column: begins a new epoch, rewrites the segments that name the old one so that they name the new, and retires the old epoch (VACUUM removes its runs once no reader can need them). Returns the runs removed now. Waits for the writers of the store and blocks them while it runs.';

CREATE VIEW sift.col_value_store_status AS
SELECT s.oid::regclass AS store, t.oid::regclass AS table_name, a.attname AS column_name,
       pg_relation_size(s.oid) AS bytes,
       (SELECT count(*) FROM pg_depend d WHERE d.classid = 'pg_class'::regclass AND d.refclassid = 'pg_class'::regclass
          AND d.refobjid = s.oid AND d.deptype = 'n') AS indexes_using,
       CASE WHEN has_table_privilege(t.oid, 'select')
            THEN (SELECT u.total_codes FROM sift.col_value_store_usage(s.oid) u LIMIT 1) END AS total_codes,
       CASE WHEN has_table_privilege(t.oid, 'select')
            THEN (SELECT u.codes FROM sift.col_value_store_usage(s.oid) u WHERE u.member IS NULL ORDER BY u.epoch DESC LIMIT 1) END AS live_codes,
       CASE WHEN has_table_privilege(t.oid, 'select')
            THEN (SELECT round(100.0 * (1 - u.codes::numeric / nullif(u.total_codes, 0)), 1)
                    FROM sift.col_value_store_usage(s.oid) u WHERE u.member IS NULL ORDER BY u.epoch DESC LIMIT 1) END AS garbage_pct
  FROM pg_class s
  JOIN pg_am am ON am.oid = s.relam AND am.amname = 'sift_values'
  JOIN pg_attribute a ON a.attrelid = s.oid AND a.attnum = 1
  LEFT JOIN pg_depend dt ON dt.classid = 'pg_class'::regclass AND dt.objid = s.oid AND dt.objsubid = 0
       AND dt.refclassid = 'pg_class'::regclass AND dt.deptype = 'a'
  LEFT JOIN pg_class t ON t.oid = dt.refobjid;
COMMENT ON VIEW sift.col_value_store_status IS 'The shared value stores (relations <table>_<column>_sift_values, managed by sift): the table and column each one serves, its size, how many indexes use it, the codes it holds, the codes that segments of the indexes still refer to (live) and the share of the others (garbage; each of these three columns reads the segments of the indexes, see sift.col_value_store_usage)';


CREATE FUNCTION sift.col_value_store_members(OUT index_name regclass, OUT table_name regclass, OUT column_name text, OUT state text, OUT key_order text,
                                             OUT duplicate_bytes float8)
RETURNS SETOF record LANGUAGE c AS 'MODULE_PATHNAME', 'sift_col_value_store_members';
COMMENT ON FUNCTION sift.col_value_store_members() IS 'The string columns of sift_col indexes that a shared value store serves or could serve: shared (the segments refer to the store), own: different key order (another index holds the column with another key order, so this one stores it again; duplicate_bytes is what it stores, estimated) or own: not yet rebuilt (a REINDEX shares it)';
