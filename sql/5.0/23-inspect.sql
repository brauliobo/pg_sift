-- Who may look, who may define: one rule for every view and inspector, and the catalogs that roles write through the sift functions.

-- A role may look at the sift objects of a table when it can read the table, maintains it (owner or MAINTAIN: the DBA who may VACUUM it) or is a member of pg_monitor.
-- Every view and every inspector (sift.index_info, sift.col_segments, sift.worker_state, ...) asks this one function; NULL (no such table) is false, but for pg_monitor.
CREATE FUNCTION sift._may_inspect(rel regclass) RETURNS boolean
	AS 'MODULE_PATHNAME', 'sift_may_inspect_sql' LANGUAGE C STABLE PARALLEL SAFE;
COMMENT ON FUNCTION sift._may_inspect(regclass) IS 'Internal: whether the caller may look at the sift objects of the table: SELECT or a maintain right on it, or membership of pg_monitor';

-- The checks of the guard triggers below, made for the role that called the outermost function (a SECURITY DEFINER function does not change it)
CREATE FUNCTION sift._require_owner(rel regclass) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_require_owner' LANGUAGE C;
CREATE FUNCTION sift._require_select(rel regclass) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_require_select' LANGUAGE C;
COMMENT ON FUNCTION sift._require_owner(regclass) IS 'Internal: errors unless the role that called the outermost function owns the table (a superuser does)';
COMMENT ON FUNCTION sift._require_select(regclass) IS 'Internal: errors unless the role that called the outermost function may read the relation';

-- sift.projections and sift.scope_follows hold what roles define on their own tables through sift.projection_* and sift.scope_follow_pairs. No role has a right on them: the functions
-- below write them as the owner of the extension, and a trigger lets only the owner of the tables a row names write it (the table of a projection; the pairs and the source of a
-- followed table, and the view that defines the pairs must be readable by the caller). A superuser, who restores them from a dump, passes.
CREATE FUNCTION sift._config_guard() RETURNS trigger LANGUAGE plpgsql SET search_path = pg_catalog, pg_temp AS $$
DECLARE
	r jsonb;
BEGIN
	FOREACH r IN ARRAY CASE TG_OP WHEN 'INSERT' THEN ARRAY[to_jsonb(NEW)] WHEN 'DELETE' THEN ARRAY[to_jsonb(OLD)] ELSE ARRAY[to_jsonb(OLD), to_jsonb(NEW)] END LOOP
		IF TG_OP = 'DELETE' AND to_regclass(r ->> 'source') IS NULL THEN
			CONTINUE;
		END IF;
		PERFORM sift._require_owner(to_regclass(r ->> 'source'));
		IF TG_TABLE_NAME = 'scope_follows' THEN
			PERFORM sift._require_owner(to_regclass(r ->> 'pairs'));
			PERFORM sift._require_select(to_regclass(r ->> 'definition'));
		END IF;
	END LOOP;
	RETURN CASE TG_OP WHEN 'DELETE' THEN OLD ELSE NEW END;
END $$;
COMMENT ON FUNCTION sift._config_guard() IS 'Internal: the trigger of sift.projections and sift.scope_follows that lets only the owner of the tables a row names write it';
CREATE TRIGGER projections_owner_guard BEFORE INSERT OR UPDATE OR DELETE ON sift.projections FOR EACH ROW EXECUTE FUNCTION sift._config_guard();
CREATE TRIGGER scope_follows_owner_guard BEFORE INSERT OR UPDATE OR DELETE ON sift.scope_follows FOR EACH ROW EXECUTE FUNCTION sift._config_guard();

-- The functions of pairs take only relations and column names: they run as the owner of the extension, the guard checks the caller. The statement trigger of a followed table fires for every
-- role that writes the table: it reads sift.scope_follows and keeps the pairs, whatever the writer may read.
ALTER FUNCTION sift.scope_follow_pairs(regclass, regclass, regclass, text, name, name) SECURITY DEFINER SET search_path = pg_catalog, pg_temp;
ALTER FUNCTION sift.scope_unfollow_pairs(regclass, regclass) SECURITY DEFINER SET search_path = pg_catalog, pg_temp;
ALTER FUNCTION sift.scope_expire_pairs(regclass, interval) SECURITY DEFINER SET search_path = pg_catalog, pg_temp;
ALTER FUNCTION sift.scope_follow_trg() SECURITY DEFINER SET search_path = pg_catalog, pg_temp;

-- The projections: the definitions are read and written as the owner of the extension. projection_create and projection_replace run the text of the caller (columns, filter, options) on an empty
-- copy of the table, so they stay the caller's and write through sift._projection_put; the readers and projection_drop take names and run as the owner of the extension.
CREATE FUNCTION sift._projection_put(p sift.projections, replace boolean) RETURNS void
	LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
BEGIN
	IF replace THEN
		UPDATE sift.projections x SET leaves = p.leaves, rollups = p.rollups, filter = p.filter, options = p.options, canonical = p.canonical, canonical_filter = p.canonical_filter
			WHERE x.name = p.name;
	ELSE
		INSERT INTO sift.projections SELECT p.*;
	END IF;
END $$;
-- the definition of a projection of a table the caller may look at, locked for a change; a row with no name where there is none
CREATE FUNCTION sift._projection_get(pname text) RETURNS sift.projections
	LANGUAGE sql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
	SELECT p FROM sift.projections p WHERE p.name = pname AND sift._may_inspect(to_regclass(p.source)) FOR UPDATE
$$;
COMMENT ON FUNCTION sift._projection_put(sift.projections, boolean) IS 'Internal: inserts or replaces a definition of sift.projections as the owner of the extension; the guard trigger checks the caller owns the table';
COMMENT ON FUNCTION sift._projection_get(text) IS 'Internal: a definition of sift.projections, for a caller that may look at its table, locked for a change';
ALTER FUNCTION sift.projection_drop(text) SECURITY DEFINER SET search_path = pg_catalog, pg_temp;
ALTER FUNCTION sift.projection_holds(oid, text) SECURITY DEFINER SET search_path = pg_catalog, pg_temp;
ALTER FUNCTION sift.projection_serves(oid, text[], jsonb) SECURITY DEFINER SET search_path = pg_catalog, pg_temp;
ALTER FUNCTION sift.projection_plan(text, text[], jsonb, boolean) SECURITY DEFINER SET search_path = pg_catalog, pg_temp;

CREATE OR REPLACE FUNCTION sift.projection_create(name text, source regclass, columns text[], rollups text DEFAULT NULL, filter text DEFAULT NULL,
												  options jsonb DEFAULT '{}') RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	src   text := sift.scope_qualified(source);
	bad   text;
	pr    record;
BEGIN
	IF (SELECT relkind FROM pg_class WHERE oid = source) NOT IN ('r', 'p') THEN
		RAISE EXCEPTION 'projection_create: "%" is not a table', src USING ERRCODE = 'wrong_object_type';
	END IF;
	IF columns IS NULL OR cardinality(columns) = 0 THEN
		RAISE EXCEPTION 'projection_create: a projection needs at least one column' USING ERRCODE = 'invalid_parameter_value';
	END IF;
	SELECT string_agg(k, ', ') INTO bad FROM jsonb_object_keys(options) k
		WHERE k IN ('projection', 'rollup_columns', 'rollup', 'rollup_dims', 'key_order', 'key_cut', 'key_cut_rows', 'order_columns', 'order_array_columns', 'order_array_group',
					'order_layout', 'point_tid_columns', 'track_changes', 'connect');
	IF bad IS NOT NULL THEN
		RAISE EXCEPTION 'projection_create: the options % belong to the index or to the rollups argument', bad USING ERRCODE = 'invalid_parameter_value',
			HINT = 'the layout of an index (key_order, key_cut, order arrays) is the layout argument of sift.projection_index';
	END IF;
	SELECT * INTO pr FROM sift.projection_probe(src, columns, rollups, filter, options);
	PERFORM sift._projection_put(ROW(name, src, columns, rollups, filter, options, pr.canonical, pr.canonical_filter)::sift.projections, false);
END $$;

CREATE OR REPLACE FUNCTION sift.projection_replace(name text, columns text[], rollups text DEFAULT NULL, filter text DEFAULT NULL, options jsonb DEFAULT '{}') RETURNS void
	LANGUAGE plpgsql AS $$
DECLARE
	old   sift.projections;
	pr    record;
	ix    oid;
	opt   text;
BEGIN
	old := sift._projection_get(name);
	IF old.name IS NULL THEN
		RAISE EXCEPTION 'projection_replace: the projection "%" is not defined', name USING ERRCODE = 'undefined_object';
	END IF;
	SELECT * INTO pr FROM sift.projection_probe(old.source, columns, rollups, filter, options);
	PERFORM sift._projection_put(ROW(old.name, old.source, columns, rollups, filter, options, pr.canonical, pr.canonical_filter)::sift.projections, true);
	IF old.rollups IS NOT DISTINCT FROM rollups AND old.options = options THEN RETURN; END IF;
	FOR ix IN SELECT * FROM sift.projection_indexes(name) LOOP
		IF sift.projection_names(ix) = ARRAY[name] THEN
			IF sift.projection_with(ARRAY[]::text[], rollups, options, '{}') IS NOT NULL THEN
				EXECUTE format('ALTER INDEX %s SET (%s)', ix::regclass, sift.projection_with(ARRAY[]::text[], rollups, options, '{}'));
			END IF;
			FOR opt IN SELECT k FROM jsonb_object_keys(old.options) k WHERE NOT options ? k LOOP
				EXECUTE format('ALTER INDEX %s RESET (%I)', ix::regclass, opt);
			END LOOP;
			IF rollups IS NULL AND old.rollups IS NOT NULL THEN EXECUTE format('ALTER INDEX %s RESET (rollup_columns)', ix::regclass); END IF;
			RAISE NOTICE 'index "%" holds the projection "%": REINDEX INDEX % writes it with the new rollups and options', ix::regclass, name, ix::regclass;
		ELSE
			RAISE NOTICE 'index "%" holds other projections too: its options stay, sift.projection_status checks "%"', ix::regclass, name;
		END IF;
	END LOOP;
END $$;

CREATE OR REPLACE VIEW sift.workers AS
	SELECT c.oid::regclass AS index, a.amname::text AS kind, w.segments, w.sealed, w.running, w.failures, w.failed_at, w.next_retry,
		   w.error_code, w.last_error,
		   CASE WHEN a.amname = 'sift_col' THEN sift.col_maintenance_due(c.oid) END AS due
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname IN ('sift', 'sift_col')
	JOIN pg_index i ON i.indexrelid = c.oid AND i.indisready
	CROSS JOIN LATERAL sift.worker_state(c.oid) w
	WHERE c.relkind = 'i' AND sift._may_inspect(i.indrelid) AND w.segments IS NOT NULL
	UNION ALL
	SELECT c.oid::regclass, a.amname::text, w.segments, w.sealed, w.running, w.failures, w.failed_at, w.next_retry,
		   w.error_code, w.last_error, NULL::text
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname = 'sift_table'
	CROSS JOIN LATERAL sift.worker_state(c.oid) w
	WHERE c.relkind IN ('r', 'm') AND sift._may_inspect(c.oid) AND w.segments IS NOT NULL;

CREATE OR REPLACE VIEW sift.col_connections AS
	SELECT c.oid::regclass AS child, l.ordinal, l.fk, l.parent_index AS parent, l.declaration, l.automatic, s.lineage, s.records, s.stamp,
		CASE WHEN f.filled = f.segments THEN 'filled'
			WHEN f.filled = 0 THEN 'unfilled'
			ELSE 'partly filled (' || f.filled || ' of ' || f.segments || ' segments)' END AS fill
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname = 'sift_col'
	JOIN pg_index i ON i.indexrelid = c.oid AND i.indisready
	CROSS JOIN LATERAL sift.col_connection_list(c.oid) l
	LEFT JOIN LATERAL sift.col_connection_state(l.parent_index) s ON true
	LEFT JOIN LATERAL sift.col_connection_fill(c.oid) f ON f.ordinal = l.ordinal
	WHERE c.relkind = 'i' AND sift._may_inspect(i.indrelid);
GRANT SELECT ON sift.col_connections TO PUBLIC;

-- The shared value stores are listed for every role (their names are in pg_class); what a store holds is read only for a table the role may look at
CREATE OR REPLACE VIEW sift.col_value_store_status AS
SELECT s.oid::regclass AS store, t.oid::regclass AS table_name, a.attname AS column_name,
       pg_relation_size(s.oid) AS bytes,
       (SELECT count(*) FROM pg_depend d WHERE d.classid = 'pg_class'::regclass AND d.refclassid = 'pg_class'::regclass
          AND d.refobjid = s.oid AND d.deptype = 'n') AS indexes_using,
       CASE WHEN sift._may_inspect(t.oid)
            THEN (SELECT u.total_codes FROM sift.col_value_store_usage(s.oid) u LIMIT 1) END AS total_codes,
       CASE WHEN sift._may_inspect(t.oid)
            THEN (SELECT u.codes FROM sift.col_value_store_usage(s.oid) u WHERE u.member IS NULL ORDER BY u.epoch DESC LIMIT 1) END AS live_codes,
       CASE WHEN sift._may_inspect(t.oid)
            THEN (SELECT round(100.0 * (1 - u.codes::numeric / nullif(u.total_codes, 0)), 1)
                    FROM sift.col_value_store_usage(s.oid) u WHERE u.member IS NULL ORDER BY u.epoch DESC LIMIT 1) END AS garbage_pct
  FROM pg_class s
  JOIN pg_am am ON am.oid = s.relam AND am.amname = 'sift_values'
  JOIN pg_attribute a ON a.attrelid = s.oid AND a.attnum = 1
  LEFT JOIN pg_depend dt ON dt.classid = 'pg_class'::regclass AND dt.objid = s.oid AND dt.objsubid = 0
       AND dt.refclassid = 'pg_class'::regclass AND dt.deptype = 'a'
  LEFT JOIN pg_class t ON t.oid = dt.refobjid;
GRANT SELECT ON sift.col_value_store_status TO PUBLIC;

-- The projections of the tables the role may look at (and those whose table is gone, for pg_monitor)
CREATE OR REPLACE VIEW sift.projection_status AS
	SELECT p.name AS projection, p.source, l.index, CASE WHEN l.index IS NULL OR l.reason IS NOT NULL THEN 'broken' ELSE 'ok' END AS state,
		   COALESCE(l.reason, CASE WHEN l.index IS NULL THEN 'no index lists it' END) AS reason, l.shared, l.size
	FROM sift.projections p LEFT JOIN LATERAL (
		SELECT i::regclass::text AS index, sift.projection_holds(i, p.name) AS reason, cardinality(sift.projection_names(i)) AS shared, pg_relation_size(i) AS size
		FROM sift.projection_indexes(p.name) i) l ON true
	WHERE sift._may_inspect(to_regclass(p.source))
	UNION ALL
	SELECT l.name, NULL, l.idx::regclass::text, 'broken', 'the projection is not defined', cardinality(sift.projection_names(l.idx)), pg_relation_size(l.idx)
	FROM sift.projection_listed() l WHERE NOT EXISTS (SELECT 1 FROM sift.projections p WHERE p.name = l.name)
		AND sift._may_inspect((SELECT x.indrelid FROM pg_index x WHERE x.indexrelid = l.idx))
	ORDER BY 1, 3;
GRANT SELECT ON sift.projection_status TO PUBLIC;

-- The status of the sift_col indexes is read by the same rule as the inspectors (sql/5.0/20-index-format.sql has the columns).
CREATE OR REPLACE VIEW sift.col_index_status AS
	SELECT c.oid::regclass AS index, m.heap AS "table",
		   pg_relation_size(c.oid) AS index_bytes, x.bytes AS segment_bytes,
		   x.free_blocks * bs.n AS free_bytes, x.reusable_blocks * bs.n AS reusable_bytes,
		   x.rows AS segment_rows, x.segments, x.max_segments, s.largest_segment_bytes, x.units, s.listed_units,
		   s.dirty_segments, s.dead_rows, round(100.0 * s.dead_rows / NULLIF(x.rows, 0), 2) AS dead_pct,
		   s.buffer_rows, w.sealed AS sealed_buffers, w.running, w.failures, w.next_retry,
		   m.last_vacuum, m.changed_rows, m.vacuum_at, m.rewrite_due AS due, m.last_request, m.blocked_by,
		   f.major || '.' || f.minor AS format, s.min_segment_format, s.max_segment_format, f.reads AS library_reads
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname = 'sift_col'
	JOIN pg_index i ON i.indexrelid = c.oid AND i.indisready
	CROSS JOIN (SELECT current_setting('block_size')::int8 AS n) bs
	CROSS JOIN LATERAL sift.col_index_info(c.oid) x
	CROSS JOIN LATERAL sift.worker_state(c.oid) w
	CROSS JOIN LATERAL sift.col_maintenance_status(c.oid) m
	CROSS JOIN LATERAL sift.index_format(c.oid) f
	CROSS JOIN LATERAL (SELECT max(bytes) FILTER (WHERE kind = 'immutable') AS largest_segment_bytes,
							   sum(units) AS listed_units, count(*) FILTER (WHERE dirty) AS dirty_segments,
							   coalesce(sum(dead_rows), 0) AS dead_rows,
							   coalesce(sum(rows) FILTER (WHERE kind <> 'immutable'), 0) AS buffer_rows,
							   min(format) AS min_segment_format, max(format) AS max_segment_format
						FROM sift.col_segments(c.oid)) s
	WHERE sift._may_inspect(i.indrelid) AND w.segments IS NOT NULL;
COMMENT ON VIEW sift.col_index_status IS 'One row per sift_col index the caller can read, for the DBA of a large table: sizes, segments against the limit, rows, dirty segments and dead rows, write buffers, the maintenance state and what blocks a rewrite (sift.col_maintenance_status), and the format: the metapage, the oldest and newest column format of its segments, and the range this library reads.';
GRANT SELECT ON sift.col_index_status TO PUBLIC;

-- One row per sift object of the database the caller may look at, whatever its access method and state: the BM25 and the sift_col indexes (an index that CREATE INDEX CONCURRENTLY is still
-- building, or a failed one, is listed as invalid), and the sift_tables. state: invalid (not valid: still building, or a build that failed), locked (a REINDEX or another session holds the object), failing
-- (the last step of its maintenance failed), maintaining (a step runs now) or ready.
CREATE VIEW sift.indexes AS
	SELECT c.oid::regclass AS relation, a.amname::text AS kind, i.indrelid::regclass AS "table", pg_relation_size(c.oid) AS bytes, w.segments, f.major || '.' || f.minor AS format,
		   CASE WHEN NOT i.indisvalid OR NOT i.indisready THEN 'invalid' WHEN w.segments IS NULL THEN 'locked'
				WHEN w.failures > 0 THEN 'failing' WHEN w.running THEN 'maintaining' ELSE 'ready' END AS state
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname IN ('sift', 'sift_col')
	JOIN pg_index i ON i.indexrelid = c.oid
	LEFT JOIN LATERAL sift.worker_state(CASE WHEN i.indisvalid AND i.indisready THEN c.oid END) w ON true
	LEFT JOIN LATERAL sift.index_format(CASE WHEN i.indisvalid AND i.indisready THEN c.oid END) f ON true
	WHERE c.relkind = 'i' AND sift._may_inspect(i.indrelid)
	UNION ALL
	SELECT c.oid::regclass, a.amname::text, c.oid::regclass, pg_relation_size(c.oid), w.segments, f.major || '.' || f.minor,
		   CASE WHEN w.segments IS NULL THEN 'locked' WHEN w.failures > 0 THEN 'failing' WHEN w.running THEN 'maintaining' ELSE 'ready' END
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname = 'sift_table'
	CROSS JOIN LATERAL sift.worker_state(c.oid) w
	CROSS JOIN LATERAL sift.index_format(c.oid) f
	WHERE c.relkind IN ('r', 'm') AND sift._may_inspect(c.oid);
COMMENT ON VIEW sift.indexes IS 'One row per sift object of the database the caller can read, whatever its access method: sift and sift_col indexes and sift_tables, with their table, size, segments, format and state (invalid, locked, failing, maintaining or ready).';
GRANT SELECT ON sift.indexes TO PUBLIC;

-- The event triggers of the extension, for the operator who has to see what runs on every DDL statement of the database and to switch it off in an emergency (a library that does not match the
-- SQL fails every statement a hook fires on). sift.ddl_hooks_enable(false) disables them all in one statement; enable sweeps what the drops left meanwhile.
COMMENT ON EVENT TRIGGER sift_col_build_lock IS 'Before CREATE INDEX: takes the table lock that the build of the change log of a sift_col index needs first, so that two builds on one table do not deadlock';
COMMENT ON EVENT TRIGGER sift_col_value_store_sql_drop IS 'After a DROP that takes an index: drops the shared value stores that no index uses any more';
COMMENT ON EVENT TRIGGER sift_publication_check IS 'After CREATE or ALTER PUBLICATION: refuses a publication of a sift_table when pg_sift is not in shared_preload_libraries';
COMMENT ON EVENT TRIGGER sift_table_ddl_check IS 'After ALTER TABLE: refuses a column that a sift_table cannot hold (more than 127), and notes a table that becomes the partition or child of another';
COMMENT ON EVENT TRIGGER sift_table_option_values_drop IS 'After a DROP: removes the rows of sift.table_option_values of the tables that went';
COMMENT ON EVENT TRIGGER sift_table_paths_drop IS 'After a DROP: removes the declared paths of sift.table_paths of the tables that went';
COMMENT ON EVENT TRIGGER sift_table_repack_guard IS 'Before CREATE TRIGGER: refuses the trigger of pg_repack on a sift_table, which the swap of its files would destroy';

CREATE VIEW sift.ddl_hooks AS
	SELECT t.evtname::text AS name, t.evtevent::text AS event, t.evttags AS tags, t.evtenabled <> 'D' AS enabled,
		   obj_description(t.oid, 'pg_event_trigger') AS effect
	FROM pg_event_trigger t
	JOIN pg_depend d ON d.classid = 'pg_event_trigger'::regclass AND d.objid = t.oid AND d.deptype = 'e'
	JOIN pg_extension e ON e.oid = d.refobjid AND e.extname = 'pg_sift'
	ORDER BY t.evtname;
COMMENT ON VIEW sift.ddl_hooks IS 'The event triggers of pg_sift: what each fires on (event and command tags), whether it is enabled and what it does. sift.ddl_hooks_enable disables or enables all of them.';
GRANT SELECT ON sift.ddl_hooks TO PUBLIC;

CREATE FUNCTION sift.ddl_hooks_enable(enable boolean) RETURNS int LANGUAGE plpgsql SET search_path = pg_catalog, pg_temp AS $$
DECLARE
	n int := 0;
	h text;
BEGIN
	FOR h IN SELECT name FROM sift.ddl_hooks LOOP
		EXECUTE format('ALTER EVENT TRIGGER %I %s', h, CASE WHEN enable THEN 'ENABLE' ELSE 'DISABLE' END);
		n := n + 1;
	END LOOP;
	IF enable THEN
		DELETE FROM sift.table_paths p WHERE NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = p.relid::oid);
		DELETE FROM sift.table_option_values v WHERE NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = v.rel::oid);
		PERFORM sift._col_value_store_drop_unused();
	END IF;
	RETURN n;
END $$;
REVOKE ALL ON FUNCTION sift.ddl_hooks_enable(boolean) FROM PUBLIC;
COMMENT ON FUNCTION sift.ddl_hooks_enable(boolean) IS 'Superuser: disables (false) or enables (true) every event trigger of pg_sift in one call and returns how many it changed; enabling also removes what the drops left meanwhile (declared paths, options, unused value stores). For a library that does not match the SQL, which fails every DDL statement a hook fires on.';

