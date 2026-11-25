-- Compatibility aliases (-- @deprecated: docs/REFERENCE.md leaves them out). Every function of 5.1 that 6.0 replaces with a unified one keeps its name, arguments and result columns, so
-- that no query, view or script of 5.1 breaks; it now calls its successor and its C entry point is no longer used (the library keeps a stub, src/core/retired.c, for the days between
-- a library swap and this update). The functions are late bound (PL/pgSQL): the unified functions that they call are created by sql/6.0/12-c-entry-points.sql.
-- Where a column cannot be read from the unified function it is NULL, and the note says which.

-- @deprecated sift.check
CREATE OR REPLACE FUNCTION sift.verify(index regclass, heapallindexed bool DEFAULT false) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_verify' LANGUAGE C STRICT VOLATILE;

-- @deprecated sift.check
CREATE OR REPLACE FUNCTION sift.col_verify(index regclass, heapallindexed bool DEFAULT false) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_col_verify' LANGUAGE C STRICT VOLATILE;

-- @deprecated sift.check; the live rows of the table as before
CREATE OR REPLACE FUNCTION sift.table_verify(tbl regclass) RETURNS bigint
	LANGUAGE plpgsql STRICT VOLATILE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
BEGIN
	PERFORM sift_internal.require_am(tbl, 'sift_table');
	PERFORM sift_internal.check_raise(tbl, false);
	RETURN (SELECT s.rows - s.dead_rows FROM sift.space(tbl) s);
END $$;

-- @deprecated sift.promote
CREATE OR REPLACE FUNCTION sift.col_promote(index regclass) RETURNS int4
	LANGUAGE plpgsql STRICT VOLATILE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
BEGIN PERFORM sift_internal.require_am(index, 'sift_col'); RETURN sift.promote(index); END $$;

-- @deprecated sift.merge
CREATE OR REPLACE FUNCTION sift.col_merge(index regclass, force bool DEFAULT false) RETURNS int4
	LANGUAGE plpgsql STRICT VOLATILE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
BEGIN PERFORM sift_internal.require_am(index, 'sift_col'); RETURN sift.merge(index, force); END $$;

-- @deprecated sift.maintain: the entry point of 5.1, which answers for Format 1 (true when the index was rewritten) and Format 2.0
CREATE OR REPLACE FUNCTION sift.col_maintain(index regclass) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_col_maintain' LANGUAGE C STRICT VOLATILE;

-- @deprecated sift.maintain
CREATE OR REPLACE FUNCTION sift.table_maintain(tbl regclass, OUT busy bool, OUT dead_rows int8, OUT promoted int4, OUT merged int4,
											   OUT blocks_before int8, OUT blocks_after int8) RETURNS record
	LANGUAGE plpgsql STRICT VOLATILE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
BEGIN
	PERFORM sift_internal.require_am(tbl, 'sift_table');
	SELECT m.busy, m.dead_rows, m.promoted, m.merged, m.blocks_before, m.blocks_after
	INTO table_maintain.busy, table_maintain.dead_rows, table_maintain.promoted, table_maintain.merged, table_maintain.blocks_before, table_maintain.blocks_after
	FROM sift.maintain(tbl) m;
END $$;

-- @deprecated sift.space, sift.status; max_segments and sum_dl are NULL
CREATE OR REPLACE FUNCTION sift.index_info(index regclass, OUT relation_blocks int8, OUT high_water int8, OUT free_blocks int8, OUT reusable_blocks int8,
										   OUT free_extents int8, OUT segments int4, OUT max_segments int4, OUT ndocs int8, OUT sum_dl int8,
										   OUT maintenance_busy bool) RETURNS record
	LANGUAGE plpgsql STRICT VOLATILE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
BEGIN
	SELECT s.relation_blocks, s.high_water, s.free_blocks, s.reusable_blocks, s.free_extents, s.parts, NULL::int4, s.rows, NULL::int8, s.maintenance_busy
	INTO index_info.relation_blocks, index_info.high_water, index_info.free_blocks, index_info.reusable_blocks, index_info.free_extents,
		 index_info.segments, index_info.max_segments, index_info.ndocs, index_info.sum_dl, index_info.maintenance_busy
	FROM sift.space(index) s;
END $$;

-- @deprecated sift.space, sift.parts: the entry point of 5.1, which answers for Format 1 (the sidecar sizes, preagg_units) and Format 2.0
CREATE OR REPLACE FUNCTION sift.col_index_info(index regclass, OUT relation_blocks int8, OUT high_water int8, OUT free_blocks int8, OUT reusable_blocks int8,
											   OUT free_extents int8, OUT segments int4, OUT max_segments int4, OUT rows int8, OUT bytes int8, OUT units int8,
											   OUT buffers int4, OUT leaves int4, OUT maintenance_busy bool, OUT simd text, OUT preagg_bytes int8,
											   OUT preagg_units int8) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_col_index_info' LANGUAGE C STRICT VOLATILE;

-- @deprecated sift.space; the entry point of 5.1, which answers for Format 1 (tail and shrink target) and Format 2.0
CREATE OR REPLACE FUNCTION sift.col_space(index regclass, OUT relation_blocks int8, OUT high_water int8, OUT used_blocks int8, OUT reusable_blocks int8,
										  OUT pending_blocks int8, OUT free_extents int4, OUT largest_free_extent int8, OUT tail_what text, OUT tail_gen int8,
										  OUT tail_end int8, OUT shrink_to int8, OUT shrink_blocks int8) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_col_space' LANGUAGE C STRICT VOLATILE;

-- @deprecated sift.space
CREATE OR REPLACE FUNCTION sift.table_space(tbl regclass, OUT relation_blocks int8, OUT high_water int8, OUT free_blocks int8, OUT reusable_blocks int8,
											OUT free_extents int8, OUT vblocks_left int8) RETURNS record
	LANGUAGE plpgsql STRICT VOLATILE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
BEGIN
	SELECT s.relation_blocks, s.high_water, s.free_blocks, s.reusable_blocks, s.free_extents, s.vblocks_left
	INTO table_space.relation_blocks, table_space.high_water, table_space.free_blocks, table_space.reusable_blocks, table_space.free_extents, table_space.vblocks_left
	FROM sift.space(tbl) s;
END $$;

-- @deprecated sift.parts; Format 1 answers as 5.1 did (superblock range, dirty flag and column format of each segment), Format 2.0 from its parts: dirty when a part has dead rows, no superblock range
CREATE OR REPLACE FUNCTION sift.col_segments(index regclass, OUT gen int8, OUT kind text, OUT rows int8, OUT bytes int8, OUT sb_lo int4, OUT sb_hi int4,
											 OUT units int4, OUT dirty bool, OUT dead_rows int8, OUT extents int4, OUT format int4, OUT preagg_bytes int8)
	RETURNS SETOF record
	LANGUAGE plpgsql STRICT VOLATILE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
BEGIN
	PERFORM sift_internal.require_am(index, 'sift_col');
	IF (SELECT f.major FROM sift.index_format(index) f) = 2 THEN
		RETURN QUERY SELECT p.part_gen, p.kind, p.rows, p.bytes, NULL::int4, NULL::int4, p.granules, p.has_dv OR p.dead_rows > 0, p.dead_rows, p.extents,
							(SELECT f.minor FROM sift.index_format(index) f), p.preagg_bytes
					 FROM sift.parts(index) p ORDER BY p.part_gen;
	ELSE
		RETURN QUERY SELECT * FROM sift_internal.v1_col_segments(index);
	END IF;
END $$;

-- @deprecated sift.parts; a part that holds rows is kind columns, as in 5.1; Format 1 answers as 5.1 did, Format 2.0 from its parts, no superblock range
CREATE OR REPLACE FUNCTION sift.table_segments(tbl regclass, OUT gen int8, OUT kind text, OUT rows int8, OUT bytes int8, OUT sb_lo int4, OUT sb_hi int4,
											   OUT units int4, OUT dead_rows int8, OUT format int4, OUT unfrozen bool, OUT min_xid xid, OUT max_xid xid)
	RETURNS SETOF record
	LANGUAGE plpgsql STRICT VOLATILE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
BEGIN
	PERFORM sift_internal.require_am(tbl, 'sift_table');
	IF (SELECT f.major FROM sift.index_format(tbl) f) = 2 THEN
		RETURN QUERY SELECT p.part_gen, CASE p.kind WHEN 'immutable' THEN 'columns' ELSE p.kind END, p.rows, p.bytes, NULL::int4, NULL::int4, p.granules, p.dead_rows,
							(SELECT f.minor FROM sift.index_format(tbl) f), p.unfrozen, p.min_xid, p.max_xid
					 FROM sift.parts(tbl) p ORDER BY p.part_gen;
	ELSE
		RETURN QUERY SELECT * FROM sift_internal.v1_table_segments(tbl);
	END IF;
END $$;

-- @deprecated sift.refusal_slugs
CREATE OR REPLACE FUNCTION sift.refusal_actions(OUT slug text, OUT action text) RETURNS SETOF record
	LANGUAGE plpgsql STRICT VOLATILE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
BEGIN RETURN QUERY SELECT r.slug, r.action FROM sift.refusal_slugs() r; END $$;

-- @deprecated sift.stats(false); the 0-argument form reads the totals of the server, as it did
CREATE OR REPLACE FUNCTION sift.stats(OUT name text, OUT value int8) RETURNS SETOF record
	LANGUAGE plpgsql STRICT VOLATILE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
BEGIN RETURN QUERY SELECT s.name, s.value FROM sift.stats(false) s; END $$;

-- @deprecated sift.migrate_plan, sift.upgrade_check: after the update nothing is required, and what is optional is listed, cheapest first
CREATE OR REPLACE FUNCTION sift.upgrade_commands(OUT kind text, OUT object regclass, OUT reason text, OUT command text, OUT required bool) RETURNS SETOF record
	LANGUAGE plpgsql STABLE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
BEGIN
	RETURN QUERY SELECT CASE f.kind WHEN 'index' THEN 'reindex' WHEN 'table' THEN 'rewrite' ELSE 'reset' END, f.object::regclass, f.advice, f.action_sql, false
				 FROM sift_internal.upgrade_findings('6.0') f
				 WHERE f.phase IN ('post_optional_upgrade', 'post_cleanup') AND f.action_sql IS NOT NULL
				   AND has_table_privilege(coalesce((SELECT i.indrelid FROM pg_index i WHERE i.indexrelid = f.object::regclass), f.object::regclass), 'MAINTAIN')
				 ORDER BY f.ord, f.object;
	RETURN QUERY SELECT 'preload', NULL::regclass, 'the first statement of each session is planned twice until the library is preloaded', sift_internal.preload_hint(), false
				 WHERE sift_internal.preload_hint() IS NOT NULL;
END $$;

-- The Format 1 diagnostics. Format 2.0 has no write-buffer arena, no tombstone log, no units and no rollup queue; their answers are what an index without them reports.

-- @deprecated sift.status: the arena of write buffers kept as references is gone
CREATE OR REPLACE FUNCTION sift.col_arena_status(OUT slots_in_use int4, OUT slots int4, OUT bytes_in_use int8, OUT rows_stored int8, OUT put_fallbacks int8,
												 OUT seals_forced int8, OUT rows_rebuilt int8, OUT segments int4, OUT bytes_taken int8) RETURNS record
	LANGUAGE sql STABLE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
	SELECT 0, 0, 0::int8, 0::int8, 0::int8, 0::int8, 0::int8, 0, 0::int8
$$;

-- @deprecated sift.parts: Format 1 keeps a stamp per segment, Format 2.0 none
CREATE OR REPLACE FUNCTION sift.col_stamps(index regclass, OUT gen bigint, OUT entries bigint, OUT stamp xid) RETURNS SETOF record
	LANGUAGE plpgsql STRICT STABLE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
BEGIN
	PERFORM sift_internal.require_am(index, 'sift_col');
	PERFORM sift_internal.require_inspect(index);
	IF (SELECT f.major FROM sift.index_format(index) f) = 1 THEN
		RETURN QUERY SELECT * FROM sift_internal.v1_col_stamps(index);
	END IF;
END $$;

-- @deprecated: the entry point of 4.2.1, which makes an index of Format 1 made by an earlier release the dependent of its two triggers
CREATE OR REPLACE FUNCTION sift.col_track_convert() RETURNS integer
	AS 'MODULE_PATHNAME', 'sift_col_track_convert' LANGUAGE C VOLATILE;

-- @deprecated sift.status, sift.health: the entry point of 5.1, which answers for Format 1 and Format 2.0
CREATE OR REPLACE FUNCTION sift.col_tracking(index regclass, OUT track_changes bool, OUT triggers bool, OUT complete bool, OUT heap_free bool) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_col_tracking' LANGUAGE C STRICT VOLATILE;

-- @deprecated sift.col_granules: the entry point of 5.1, which answers for Format 1 (the pre-aggregates of each unit)
CREATE OR REPLACE FUNCTION sift.col_unit_agg(index regclass, leaf text, OUT segment int8, OUT sb int4, OUT rows int4, OUT exact bool, OUT nvals int8,
											 OUT nnull int8, OUT min text, OUT max text, OUT sum numeric, OUT sumsq numeric, OUT sum_float float8) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_unit_aggs_sql' LANGUAGE C VOLATILE;

-- @deprecated sift.col_granules: the entry point of 5.1, which answers for Format 1 (the histogram of each unit)
CREATE OR REPLACE FUNCTION sift.col_unit_hist(index regclass, dim text, measure text DEFAULT NULL, OUT segment int8, OUT sb int4, OUT exact bool, OUT key text,
											  OUT n int8, OUT nn int8, OUT sum numeric, OUT sum_float float8) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_unit_hists_sql' LANGUAGE C VOLATILE;

-- @deprecated sift.activity: pre-aggregates are rewritten by the maintenance step, no request queue is kept
CREATE OR REPLACE FUNCTION sift.col_rollup_counters(OUT requested int8, OUT refreshed int8, OUT skipped int8, OUT dropped int8, OUT queued int4,
												   OUT refreshing int4) RETURNS record
	LANGUAGE sql STABLE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
	SELECT 0::int8, 0::int8, 0::int8, 0::int8, 0, 0
$$;

-- @deprecated sift.activity
CREATE OR REPLACE FUNCTION sift.col_rollup_queue(OUT index regclass, OUT segment int8, OUT units int4, OUT requested_at timestamptz, OUT state text) RETURNS SETOF record
	LANGUAGE sql STABLE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
	SELECT NULL::regclass, NULL::int8, NULL::int4, NULL::timestamptz, NULL::text WHERE false
$$;

-- @deprecated sift.maintain
CREATE OR REPLACE FUNCTION sift.col_rollup_refresh(index regclass) RETURNS int4
	LANGUAGE sql STRICT VOLATILE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$ SELECT 0 $$;

-- @deprecated sift.explain_why: the scans of this session the pre-aggregates did not answer
CREATE OR REPLACE FUNCTION sift.col_rollup_misses(OUT rel regclass, OUT reason text, OUT fixable bool, OUT scans int8, OUT units int8) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_rollup_misses' LANGUAGE C STRICT VOLATILE;

-- @deprecated sift.space
CREATE OR REPLACE FUNCTION sift.col_shrink_history(index regclass, OUT shrink_blocked_by text, OUT last_shrink timestamptz) RETURNS record
	LANGUAGE plpgsql STRICT STABLE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
BEGIN
	SELECT s.shrink_blocked_by, s.last_shrink INTO col_shrink_history.shrink_blocked_by, col_shrink_history.last_shrink FROM sift.space(index) s;
END $$;

-- @deprecated sift.index_choices: the advice machinery of 5.1 is gone, the build says what it chose
CREATE OR REPLACE FUNCTION sift.index_advice(index regclass, OUT leaf text, OUT kind text, OUT held text, OUT sampled int4, OUT share int4, OUT advice text)
	RETURNS SETOF record
	LANGUAGE sql STRICT STABLE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
	SELECT NULL::text, NULL::text, NULL::text, NULL::int4, NULL::int4, NULL::text WHERE false
$$;

-- @deprecated sift.index_choices: key_cut was replaced by key_order = 'auto'
CREATE OR REPLACE FUNCTION sift.key_cut_decision(index regclass, OUT leaf text, OUT units float8, OUT sampled float8, OUT reads float8, OUT ideal float8,
												 OUT growth_pct float8, OUT decision text, OUT reason text) RETURNS record
	LANGUAGE sql STRICT STABLE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
	SELECT NULL::text, NULL::float8, NULL::float8, NULL::float8, NULL::float8, NULL::float8, 'none', 'key_cut was replaced by key_order = ''auto'', which sift.index_choices reports'
$$;

-- The shared value store of a column (5.0 and 5.1) is gone: 6.0 neither writes nor reads it (a segment that still refers to one is refused with a REINDEX hint, never quarantined). The three functions and
-- the view sift.col_value_store_status of 5.1 stay, so that no query or view of 5.1 breaks; the view lists only the stores of the keys of a scope, which these answer as empty.

-- @deprecated: no shared value store is kept; no rows
CREATE OR REPLACE FUNCTION sift.col_value_store_usage(store regclass, OUT epoch int8, OUT member regclass, OUT segments int4, OUT codes int8, OUT total_codes int8) RETURNS SETOF record
	LANGUAGE sql STABLE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
	SELECT NULL::int8, NULL::regclass, NULL::int4, NULL::int8, NULL::int8 WHERE false
$$;

-- @deprecated: no shared value store is kept; no rows
CREATE OR REPLACE FUNCTION sift.col_value_store_members(OUT index_name regclass, OUT table_name regclass, OUT column_name text, OUT state text, OUT key_order text, OUT duplicate_bytes float8)
	RETURNS SETOF record
	LANGUAGE sql STABLE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$
	SELECT NULL::regclass, NULL::regclass, NULL::text, NULL::text, NULL::text, NULL::float8 WHERE false
$$;

-- @deprecated: no shared value store is kept; nothing to compact, 0 runs removed
CREATE OR REPLACE FUNCTION sift.col_value_store_compact(tbl regclass, col name) RETURNS int8
	LANGUAGE sql VOLATILE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $$ SELECT 0::int8 $$;

COMMENT ON FUNCTION sift.verify(regclass, bool) IS 'Deprecated: sift.check. Raises the first problem of the index.';
COMMENT ON FUNCTION sift.col_verify(regclass, bool) IS 'Deprecated: sift.check. Raises the first problem of the index.';
COMMENT ON FUNCTION sift.table_verify(regclass) IS 'Deprecated: sift.check. Raises the first problem of the table and returns its live rows.';
COMMENT ON FUNCTION sift.col_promote(regclass) IS 'Deprecated: sift.promote.';
COMMENT ON FUNCTION sift.col_merge(regclass, bool) IS 'Deprecated: sift.merge.';
COMMENT ON FUNCTION sift.col_maintain(regclass) IS 'Deprecated: sift.maintain. True when the index was rewritten.';
COMMENT ON FUNCTION sift.table_maintain(regclass) IS 'Deprecated: sift.maintain.';
COMMENT ON FUNCTION sift.index_info(regclass) IS 'Deprecated: sift.space and sift.status. max_segments and sum_dl are NULL.';
COMMENT ON FUNCTION sift.col_index_info(regclass) IS 'Deprecated: sift.space and sift.parts. max_segments, maintenance_busy and the preagg columns are NULL for Format 2.0.';
COMMENT ON FUNCTION sift.col_space(regclass) IS 'Deprecated: sift.space. The tail and the shrink target are NULL for Format 2.0.';
COMMENT ON FUNCTION sift.table_space(regclass) IS 'Deprecated: sift.space.';
COMMENT ON FUNCTION sift.col_segments(regclass) IS 'Deprecated: sift.parts. Format 2.0 has no superblock range: NULL.';
COMMENT ON FUNCTION sift.table_segments(regclass) IS 'Deprecated: sift.parts. Format 2.0 has no superblock range: NULL.';
COMMENT ON FUNCTION sift.refusal_actions() IS 'Deprecated: sift.refusal_slugs, which has the action column.';
COMMENT ON FUNCTION sift.stats() IS 'Deprecated: sift.stats(false), the totals of the server; sift.stats(true) has the figures of this session.';
COMMENT ON FUNCTION sift.upgrade_commands() IS 'Deprecated: sift.migrate_plan and sift.upgrade_check. Nothing is required after the update; the rows are the optional steps, for the indexes and tables the caller owns or may maintain.';
COMMENT ON FUNCTION sift.col_arena_status() IS 'Deprecated: the arena of write buffers is gone; all zero.';
COMMENT ON FUNCTION sift.col_stamps(regclass) IS 'Deprecated: the segment stamps of Format 1; no rows for Format 2.0.';
COMMENT ON FUNCTION sift.col_track_convert() IS 'Deprecated: the number of indexes of Format 1 whose triggers were made their dependents; 0 when none is left.';
COMMENT ON FUNCTION sift.col_tracking(regclass) IS 'Deprecated: sift.status and sift.health. Whether the log is complete and read without the heap.';
COMMENT ON FUNCTION sift.col_unit_agg(regclass, text) IS 'Deprecated: sift.col_granules. The units of Format 1; no rows for Format 2.0.';
COMMENT ON FUNCTION sift.col_unit_hist(regclass, text, text) IS 'Deprecated: sift.col_granules. The units of Format 1; no rows for Format 2.0.';
COMMENT ON FUNCTION sift.col_rollup_counters() IS 'Deprecated: sift.activity. All zero.';
COMMENT ON FUNCTION sift.col_rollup_queue() IS 'Deprecated: sift.activity. No rows.';
COMMENT ON FUNCTION sift.col_rollup_refresh(regclass) IS 'Deprecated: sift.maintain. 0.';
COMMENT ON FUNCTION sift.col_rollup_misses() IS 'Deprecated: sift.explain_why. The scans of this session the pre-aggregates did not answer.';
COMMENT ON FUNCTION sift.col_shrink_history(regclass) IS 'Deprecated: sift.space.';
COMMENT ON FUNCTION sift.index_advice(regclass) IS 'Deprecated: sift.index_choices. No rows.';
COMMENT ON FUNCTION sift.key_cut_decision(regclass) IS 'Deprecated: key_order = ''auto'' and sift.index_choices. Decision none.';
COMMENT ON FUNCTION sift.table_set_paths(regclass, name, text) IS 'Deprecated: sift.table_set_option(tbl, ''paths'', ...).';
COMMENT ON FUNCTION sift.col_value_store_usage(regclass) IS 'Deprecated: the shared value store of a column is gone in 6.0. No rows.';
COMMENT ON FUNCTION sift.col_value_store_members() IS 'Deprecated: the shared value store of a column is gone in 6.0. No rows.';
COMMENT ON FUNCTION sift.col_value_store_compact(regclass, name) IS 'Deprecated: the shared value store of a column is gone in 6.0. Nothing to compact; 0.';
