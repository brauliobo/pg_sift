-- The unified API: one function for the three access methods (sift, sift_col, sift_table) and for both column formats; the library reads the access method of the relation.
-- These are the only functions of the release that name a C symbol that 5.1 did not have (C-CONTRACT.md lists the symbols with their rows): the unified functions below, the ledger of the shared
-- memory pools and the two quantile functions. Everything else in sql/6.0 is SQL over these and over what 5.1 already had.

-- Space of the file: used, reusable and pending blocks, free extents and the shrink state of a sift, sift_col or sift_table (vblocks_left: the virtual TID blocks not handed out, NULL for an index).
CREATE OR REPLACE FUNCTION sift.space(rel regclass)
RETURNS TABLE (am text, relation_blocks int8, high_water int8, used_blocks int8, free_blocks int8, reusable_blocks int8, pending_blocks int8,
			   free_extents int8, largest_free_extent int8, parts int4, rows int8, dead_rows int8, bytes int8, maintenance_busy bool,
			   last_shrink timestamptz, shrink_blocked_by text, vblocks_left int8)
	AS 'MODULE_PATHNAME', 'sift_space' LANGUAGE C STRICT STABLE;
COMMENT ON FUNCTION sift.space(regclass) IS 'The space of a sift index or sift_table in one row: blocks in use, reusable and pending, free extents, parts, rows, dead rows, whether a maintenance step runs, why the end of the file is not cut (standby, snapshot or lock) and, for a sift_table, the virtual TID blocks left. Works on Format 1 and Format 2.0.';

-- The parts (Format 2.0) or segments (Format 1) of a sift_col index or sift_table, one row each. A sift (BM25) index has sift.segments.
CREATE OR REPLACE FUNCTION sift.parts(rel regclass)
RETURNS TABLE (part_id int8, part_gen int8, level int2, kind text, rows int8, dead_rows int8, bytes int8, granules int4, extents int4,
			   has_dv bool, unfrozen bool, min_xid xid, max_xid xid, preagg_bytes int8)
	AS 'MODULE_PATHNAME', 'sift_parts' LANGUAGE C STRICT STABLE;
COMMENT ON FUNCTION sift.parts(regclass) IS 'The parts of a sift_col index or sift_table, one row each: level, kind, rows, dead rows, bytes, granules, extents, whether a deletion vector holds dead rows, the transaction ids that still need freezing, and the bytes of the pre-aggregates. A Format 1 segment is listed as a part.';

-- One maintenance step for a sift, sift_col or sift_table now, in this session: the buffers promoted, the parts merged, the dead rows cleared, the free end of the file returned. busy: the lock is held and nothing was done.
CREATE OR REPLACE FUNCTION sift.maintain(rel regclass)
RETURNS TABLE (busy bool, dead_rows int8, promoted int4, merged int4, blocks_before int8, blocks_after int8)
	AS 'MODULE_PATHNAME', 'sift_maintain' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.maintain(regclass) IS 'One maintenance step of a sift, sift_col or sift_table, now and in this session (for a job: pg_cron): buffers promoted, parts merged, dead rows cleared, free end of the file returned. busy: the lock is held and nothing was done. Background maintenance does the same by itself.';

-- Everything about one relation (or every one the caller may look at, rel NULL) from the metapage and the directory alone: no data page is read, so a scrape takes milliseconds on a large table.
-- A relation that is invalid, locked, quarantined or in Format Major 1 is a row too, with what can be said of it.
CREATE OR REPLACE FUNCTION sift.status(rel regclass DEFAULT NULL)
RETURNS TABLE (relation regclass, table_name regclass, am text, format_major int2, format_minor int2, format_note text, state text, quarantined bool, quarantine_reason text,
			   live_rows int8, dead_rows int8, dirty_rows int8, dead_pct numeric(5,2),
			   size_bytes int8, heap_bytes int8, b_per_row numeric(8,2), compression_ratio numeric(6,2), util_pct numeric(5,2),
			   data_bytes int8, meta_bytes int8, scope_bytes int8, skip_bytes int8, free_bytes int8, reusable_bytes int8, slack_bytes int8,
			   partitions int4, l0_slabs int4, l0_parts int4, l1_parts int4, l2_parts int4, hot_parts int4, cold_parts int4, granules int8, scopes int2, vblocks_left int8,
			   maint_running bool, maint_due text, maint_failures int4, last_maint_at timestamptz, last_shrink_at timestamptz, shrink_blocked_by text,
			   shm_manifest_cached bool, shm_manifest_bytes int8, shm_hit_pct numeric(5,2))
	AS 'MODULE_PATHNAME', 'sift_status' LANGUAGE C STABLE;
COMMENT ON FUNCTION sift.status(regclass) IS 'The state of a sift index or sift_table (or of every one the caller may look at): format, state, rows, dead rows, footprint, the parts by level, the scopes, the maintenance state and the shared memory it holds. Reads the metapage and the directory only. NULL means not applicable or not known, 0 means measured zero.';

-- The 8,192-row granules of a sift_col index or sift_table, one row each.
CREATE OR REPLACE FUNCTION sift.col_granules(rel regclass)
RETURNS TABLE (part_id int8, part_gen int8, granule int4, nrows int4, dead_rows int4, bytes int4, all_frozen bool, has_preagg bool, has_tstride bool, min_xid xid, max_xid xid)
	AS 'MODULE_PATHNAME', 'sift_col_granules' LANGUAGE C STRICT STABLE;
COMMENT ON FUNCTION sift.col_granules(regclass) IS 'The granules of a Format 2.0 sift_col index or sift_table, one row each: rows, dead rows, bytes, whether all rows are frozen, whether the granule holds pre-aggregates and a time stride.';

-- The scopes of a sift_col index or a sift_table: where each one is kept, its keys, the levels of its summary and the keys waiting for a refresh.
CREATE OR REPLACE FUNCTION sift.scope_stats(index regclass)
RETURNS TABLE (scope_ord int2, scope_name text, header_blk int8, meta_gen int8, nkeys int8, l0_slices int4, l1_nodes int4, l2_roots int4, topk_keys int4,
			   bytes int8, dirty_keys int4, clustered bool)
	AS 'MODULE_PATHNAME', 'sift_scope_stats' LANGUAGE C STRICT STABLE;
COMMENT ON FUNCTION sift.scope_stats(regclass) IS 'The Entity-Once scopes of a sift_col index or a sift_table, one row each: the keys, the levels of the summary, the bytes, the keys whose summary waits for a refresh, and whether the entity rows are clustered by the key.';

-- Rewrites a sift_table of Format 1 in Format 2.0, in place and under an ACCESS EXCLUSIVE lock, like VACUUM FULL. False when it is in Format 2.0 already (or empty). A sift_col index is rebuilt with REINDEX INDEX CONCURRENTLY.
CREATE OR REPLACE FUNCTION sift.upgrade_table(rel regclass) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_upgrade_table' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.upgrade_table(regclass) IS 'Rewrites a Format 1 sift_table in Format 2.0 under an ACCESS EXCLUSIVE lock, as VACUUM FULL does, and returns true; false (and a notice) when the table is in Format 2.0 already. Needs MAINTAIN on the table. Optional: a Format 1 table is fully supported.';

-- The queue and the pace of the background maintenance, for sift.activity: the jobs that run and those that wait (the worker, the part, the phase, the priority), and what the workers are doing to the server.
CREATE OR REPLACE FUNCTION sift_internal.maint_queue(OUT pid int4, OUT relation oid, OUT part_id int8, OUT job text, OUT phase text, OUT priority int4,
										  OUT done int8, OUT total int8, OUT bytes_written int8, OUT started_at timestamptz, OUT queued bool)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_maint_queue' LANGUAGE C STABLE;
CREATE OR REPLACE FUNCTION sift_internal.maint_pacer(OUT workers_active int4, OUT workers_max int4, OUT pacer_mode text, OUT pacer_delay_ms int4, OUT oltp_active_backends int4)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_maint_pacer' LANGUAGE C STABLE;
COMMENT ON FUNCTION sift_internal.maint_queue() IS 'Internal: the running and queued maintenance jobs of the database (the support of sift.activity)';
COMMENT ON FUNCTION sift_internal.maint_pacer() IS 'Internal: the workers and the adaptive pace of the background maintenance (the support of sift.activity)';

-- The result of these two gains a column: action, the statement or setting to change, read from the table of the refusals. A view that reads them fails the DROP and the update says which one
-- (pg_sift_upgrade_check.sql names it beforehand); nothing in pg_sift depends on them.
DO $$ BEGIN DROP FUNCTION IF EXISTS sift.explain_why(text); END $$;
CREATE OR REPLACE FUNCTION sift.explain_why(query text, OUT arm text, OUT used bool, OUT slug text, OUT reason text, OUT action text, OUT scan_cost float8, OUT normal_cost float8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_agg_explain_why' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.explain_why(text) IS 'Why each candidate path of the aggregate pushdown kept or left the normal plan for one SELECT, planned and not run: one row per arm with the first reason it gave up for and what to change (action); slug, reason and action are NULL for an arm that is used. Needs the privilege to select from the tables of the query.';

DO $$ BEGIN DROP FUNCTION IF EXISTS sift.refusal_slugs(); END $$;
CREATE OR REPLACE FUNCTION sift.refusal_slugs(OUT slug text, OUT class text, OUT reason text, OUT action text)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_agg_refusal_slugs' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.refusal_slugs() IS 'The slugs that sift.explain_why and sift.stats() name, with their class (limit: the query has more of something than a fixed limit of the extension allows; semantic: otherwise), the reason and what to change.';

-- Functions of 5.1 that keep their signature and now work on every access method and format (the library dispatches on the access method of the relation).
COMMENT ON FUNCTION sift.promote(regclass) IS 'Seals the write buffer of a sift, sift_col or sift_table into a part now and returns how many were promoted. Background maintenance does the same by itself.';
COMMENT ON FUNCTION sift.merge(regclass, bool) IS 'Merges the parts or segments of a sift, sift_col or sift_table now, in this session, and returns how many were merged; force merges when nothing is due.';
COMMENT ON FUNCTION sift.check(regclass, bool, int) IS 'The problems of the sift indexes and tables the caller may maintain (or of one), a row each with its segment or part; no row when sound. heapallindexed also compares every heap row with the index. Works on a standby and on Format 1 and Format 2.0.';
COMMENT ON FUNCTION sift.index_format(regclass) IS 'The metapage format of a sift index, a sift_col index or a sift_table, Format 1 and Format 2.0: major and minor; long_words, for a sift index, tells whether it was built by 5.0 or later; reads says what the loaded library does with it. Never fails on a format it can read.';
COMMENT ON EVENT TRIGGER sift_table_ddl_check IS 'After ALTER TABLE: refuses a column that a sift_table cannot hold (more than 1,024), and notes a table that becomes the partition or child of another';

-- The five pools of the shared memory budget (sift.shared_cache_kb), one row each in the order of the ledger: the share of the limit, what is held, the most that was held, the refusals and the pool
-- entries that were evicted to make room. sift.shared_memory() keeps its 5.1 columns; this one is the ledger that sift.health and sift.activity read.
CREATE OR REPLACE FUNCTION sift.shared_memory_pools(OUT pool text, OUT limit_bytes int8, OUT used_bytes int8, OUT peak_bytes int8, OUT util_pct numeric(5,2), OUT evictions int8, OUT refusals int8,
													OUT shared bool, OUT reason text)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_shared_memory_pools' LANGUAGE C STABLE;
COMMENT ON FUNCTION sift.shared_memory_pools() IS 'The five pools of the shared memory budget (manifest, agg_lookup, agg_dimmap, col_dict, col_leaf), one row each: limit and bytes held, the peak, how full the pool is (NULL for a pool with no limit), the evictions and the refusals since the server started, and whether the pool is shared by the backends (reason says why not).';

-- sift.approx_percentile and sift.approx_quantiles: the quantile of a numeric or date/time column of a sift_col index or its table, from the SEC_KLL sketches of the index when the option
-- sketches = 'kll(col)' made them and at most 1% of the rows are dead, pending or in a part without one; exact (percentile_disc) otherwise. Goes in sql/6.0/15-unified-api.sql.

CREATE OR REPLACE FUNCTION sift.approx_percentile(rel regclass, col text, p float8, where_sql text DEFAULT NULL)
RETURNS float8
	AS 'MODULE_PATHNAME', 'sift_approx_percentile' LANGUAGE C STABLE PARALLEL RESTRICTED;
COMMENT ON FUNCTION sift.approx_percentile(regclass, text, float8, text) IS 'The least value of a column whose share of the rows is at least p (0 to 1): from the quantile sketches of the sift_col index when it keeps them (sketches = ''kll(col)'') and at most 1% of the rows are dead, pending or in a part without one, else exact; where_sql is a filter and always answers exactly. NaN, NULL and infinite dates are no values. A date is days and a timestamp microseconds since 2000-01-01.';

CREATE OR REPLACE FUNCTION sift.approx_quantiles(rel regclass, col text, ps float8[], where_sql text DEFAULT NULL)
RETURNS float8[]
	AS 'MODULE_PATHNAME', 'sift_approx_quantiles' LANGUAGE C STABLE PARALLEL RESTRICTED;
COMMENT ON FUNCTION sift.approx_quantiles(regclass, text, float8[], text) IS 'sift.approx_percentile for each fraction of ps, from one pass over the sketches (or one exact query); the array has one value for each fraction, in order.';

-- The options of the format of a sift_table, a row each: the text the table stores (NULL when it sets none), what its format has for a table that sets none, and the setting of the cluster that stands for
-- the option (NULL when none does). sift.table_options reads them; the settings sift.table_* of 5.1 are gone, so no option is looked up by one.
CREATE OR REPLACE FUNCTION sift._table_options(tbl regclass, OUT name text, OUT stored text, OUT default_value text, OUT setting text)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_table_options' LANGUAGE C STABLE STRICT;
COMMENT ON FUNCTION sift._table_options(regclass) IS 'The options of the format of a sift_table: the text the table stores, the default of its format and the setting of the cluster that stands for it (the support of sift.table_options).';

CREATE OR REPLACE FUNCTION sift.table_options(tbl regclass DEFAULT NULL)
	RETURNS TABLE (rel regclass, name text, value text, source text, default_value text)
	LANGUAGE sql STABLE SECURITY INVOKER SET search_path = pg_catalog, sift AS $$
	WITH t AS MATERIALIZED (
		SELECT c.oid AS relid, c.oid::regclass AS rel
		FROM pg_class c
		JOIN pg_am a ON a.oid = c.relam AND a.amname = 'sift_table'
		WHERE c.relkind IN ('r', 'm') AND (tbl IS NULL OR c.oid = tbl)
		  AND has_table_privilege(c.oid, 'SELECT, INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES, TRIGGER'))
	SELECT t.rel, o.name, coalesce(o.stored, o.default_value),
		   CASE WHEN o.stored IS NOT NULL THEN 'table' WHEN s.source <> 'default' THEN 'setting' ELSE 'default' END,
		   coalesce(s.boot_val, o.default_value)
	FROM t
	CROSS JOIN LATERAL sift._table_options(t.relid) WITH ORDINALITY AS o(name, stored, default_value, setting, ord)
	LEFT JOIN pg_settings s ON s.name = o.setting
	ORDER BY t.rel::text, o.ord
$$;
COMMENT ON FUNCTION sift.table_options(regclass) IS 'The per-table options of the sift_tables the caller may see (or of one): value in force, source (table, setting, default) and built-in default. A setting is only in the source of an option that one stands for.';
