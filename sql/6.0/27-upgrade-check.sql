-- The preflight of the update to 6.0, one function for the three things that read it: sift.upgrade_check (the findings), sift.migrate_plan (the same findings as an ordered runbook) and
-- pg_sift_upgrade_check.sql (both, for a database that still has the 5.1 extension: this part is the whole of it). It reads the catalogs and the metapages and changes nothing; it runs on a 5.1 library and on a 6.0
-- one (sift.index_format reads both formats), so that the same text checks before and after the library is swapped.
-- Seven checks, in this order: version, setting, option, index and table (the format), dependency, replication, test.
--   blocker: the update or the next restart fails until it is done.      warn: look at it.      info: optional, nothing is wrong.      ok: nothing to do.
-- A relation of Format Major 1 and an option that 5.1 stored are info: 6.0 reads and writes them as 5.1 did, and the upgrade to Format 2.0 is yours to schedule.
-- phase says where a finding lands in the runbook: pre_guc, pre_dep, upgrade_ext, post_optional_upgrade, post_cleanup.
CREATE SCHEMA IF NOT EXISTS sift_internal;

-- The sift.* lines of the configuration files, for the roles that may read pg_file_settings (superusers and pg_read_all_settings); any other role gets no rows, not a permission error.
CREATE OR REPLACE FUNCTION sift_internal.file_settings() RETURNS TABLE (name text, setting text, sourcefile text, sourceline int4)
LANGUAGE plpgsql STABLE SECURITY INVOKER SET search_path = pg_catalog AS $fn$
BEGIN
	IF pg_has_role(current_user, 'pg_read_all_settings', 'USAGE') THEN
		RETURN QUERY SELECT f.name, f.setting, f.sourcefile, f.sourceline FROM pg_file_settings f WHERE f.name LIKE 'sift.%';
	END IF;
END
$fn$;

CREATE OR REPLACE FUNCTION sift_internal.upgrade_findings(target text DEFAULT NULL)
RETURNS TABLE (ord int, phase text, object text, kind text, detected text, status text, advice text, action_sql text, size_bytes int8)
LANGUAGE plpgsql SECURITY INVOKER SET search_path = pg_catalog, sift AS $fn$
DECLARE
	-- the settings that 6.0 no longer has, each with what replaces it: the 90 settings of 5.1 that 6.0 removes or folds (the GPU settings stay: Vulkan is kept), and the three that 4.2 and 5.0 removed
	removed CONSTANT jsonb := '{
		"sift.agg_case_filter": "a test-build setting only",
		"sift.agg_child_first": "a test-build setting only",
		"sift.agg_combo_cache": "sized from work_mem and hash_mem_multiplier",
		"sift.agg_connect_found": "a test-build setting only",
		"sift.agg_dead_delta": "tuned automatically",
		"sift.agg_dense": "the code path is gone",
		"sift.agg_dimmap_kb": "use sift.shared_cache_kb",
		"sift.agg_dist_stream": "a test-build setting only",
		"sift.agg_float_step": "the code path is gone",
		"sift.agg_force": "a test-build setting only",
		"sift.agg_fuse": "the code path is gone",
		"sift.agg_join_filter": "a test-build setting only",
		"sift.agg_key_skew": "a test-build setting only",
		"sift.agg_left_lateral": "a test-build setting only",
		"sift.agg_lookup_cache_kb": "use sift.backend_cache_kb",
		"sift.agg_lookup_columns": "a test-build setting only",
		"sift.agg_lookup_max_kb": "sized from work_mem",
		"sift.agg_lookup_max_rows": "no replacement",
		"sift.agg_lookup_pushdown": "a test-build setting only",
		"sift.agg_lookup_shared_kb": "use sift.shared_cache_kb",
		"sift.agg_par_min_rows": "tuned automatically",
		"sift.agg_param_set_size": "sized from work_mem",
		"sift.agg_preagg": "a test-build setting only",
		"sift.agg_preagg_distinct": "a test-build setting only",
		"sift.agg_probe_direct": "the code path is gone",
		"sift.agg_probe_rewrite": "the code path is gone",
		"sift.agg_rollup_cost": "tuned automatically",
		"sift.agg_rows": "a test-build setting only",
		"sift.agg_scale_horizon": "tuned automatically",
		"sift.agg_scope": "a test-build setting only",
		"sift.agg_semi_lut": "a test-build setting only",
		"sift.agg_semi_lut_size": "sized from work_mem",
		"sift.agg_share_lookups": "a test-build setting only",
		"sift.agg_super": "the code path is gone",
		"sift.agg_two_level": "the code path is gone",
		"sift.agg_verdict_cache": "the code path is gone",
		"sift.cluster_advice": "call sift.cluster_advice() when you want the advice",
		"sift.col_auto_connect": "tuned automatically",
		"sift.col_auto_merge": "use sift.async_maintenance",
		"sift.col_auto_merge_buffer_pct": "tuned automatically",
		"sift.col_auto_merge_dirty_pct": "no replacement",
		"sift.col_auto_merge_fragmentation": "tuned automatically",
		"sift.col_auto_merge_partition_kb": "derived from maintenance_work_mem",
		"sift.col_auto_merge_pass_mb": "derived from maintenance_work_mem",
		"sift.col_auto_merge_wal_pct": "use sift.maintenance_wal_rate",
		"sift.col_build_source": "a test-build setting only",
		"sift.col_connect_refresh_pct": "tuned automatically",
		"sift.col_dict_shared_cache_kb": "use sift.shared_cache_kb",
		"sift.col_format_minor": "Format 2.0 has one version",
		"sift.col_leaf_cache_kb": "use sift.shared_cache_kb",
		"sift.col_rollup_on_demand": "tuned automatically",
		"sift.col_rollup_worker": "use sift.async_maintenance",
		"sift.col_scope_refresh_pct": "tuned automatically",
		"sift.col_unit_floor": "granules hold 8192 rows",
		"sift.col_unit_rows": "granules hold 8192 rows",
		"sift.col_value_store": "tuned automatically",
		"sift.col_wbuf_key_range": "always on",
		"sift.col_wbuf_ref": "level 0 parts replace it",
		"sift.enable_col": "sift.enable_col_scan and sift.enable_agg are the switches",
		"sift.enable_count": "use sift.enable_scan",
		"sift.enable_topn": "use sift.enable_agg",
		"sift.eval_methods": "no replacement",
		"sift.plan_scale": "no replacement",
		"sift.prefetch": "a test-build setting only",
		"sift.prefix_cache_size": "use sift.backend_cache_kb",
		"sift.scope_max_multiplicity": "a scope has no fan-out",
		"sift.search_chunk_sbs": "tuned automatically",
		"sift.table_async_promote": "use sift.async_maintenance",
		"sift.table_auto_defrag": "use sift.async_maintenance",
		"sift.table_auto_defrag_budget": "tuned automatically",
		"sift.table_auto_defrag_coalesce": "tuned automatically",
		"sift.table_auto_defrag_dead_pct": "it has had no effect since 4.0",
		"sift.table_auto_defrag_delay": "tuned automatically",
		"sift.table_auto_defrag_free_pct": "tuned automatically",
		"sift.table_auto_defrag_indexes": "tuned automatically",
		"sift.table_auto_defrag_max_active": "tuned automatically",
		"sift.table_auto_defrag_wal_rate": "use sift.maintenance_wal_rate",
		"sift.table_backpressure": "no replacement",
		"sift.table_bulk_groups": "tuned automatically",
		"sift.table_compression": "set the compression option of the table with sift.table_set_option",
		"sift.table_dead_rewrite_pct": "use sift.dead_rewrite_pct",
		"sift.table_dict_lookup": "tuned automatically",
		"sift.table_hot_update": "tuned automatically",
		"sift.table_inline_promote": "tuned automatically",
		"sift.table_key_table": "tuned automatically",
		"sift.table_wide_rows": "tuned automatically",
		"sift.table_write_buffer_limit": "a Format 1 table keeps its stored option",
		"sift.tomb_log_limit": "deletes are kept in deletion vectors",
		"sift.topn_runs": "the code path is gone",
		"sift.topn_walk": "the code path is gone",
		"sift.topn_zone_keys": "the code path is gone",
		"sift.verdict_cache_kb": "the verdict cache is gone",
		"sift.vis_page_cache": "a test-build setting only"
}';
	-- settings of the test build that a release build does not define (the others begin with sift.test_)
	test_only CONSTANT text[] := ARRAY['sift.grouped_min_df', 'sift.table_init_gate', 'sift.table_max_segments', 'sift.table_seal_gate', 'sift.table_vblock_floor'];
	-- the reloptions of a sift_col index that 6.0 documents no more, each with where Format 2.0 takes its meaning from
	col_removed CONSTANT jsonb := '{
		"key_cut": "key_order",
		"order_array_columns": "key_order",
		"order_array_group": "key_order",
		"order_layout": "key_order",
		"rollup_columns": "preagg",
		"rollup_leaves": "preagg",
		"calendar_columns": "preagg",
		"rollup_dims": "preagg",
		"rollup": "preagg",
		"key_cut_rows": "tuned automatically",
		"order_columns": "tuned automatically",
		"point_columns": "tuned automatically",
		"cube_minmax": "tuned automatically",
		"key_table": "tuned automatically",
		"dict_lookup": "tuned automatically",
		"array_lengths": "tuned automatically",
		"count_columns": "tuned automatically",
		"key_columns": "tuned automatically",
		"point_tid_columns": "tuned automatically",
		"return_columns": "tuned automatically",
		"superunit": "tuned automatically",
		"rollup_on_demand": "tuned automatically",
		"cluster_advice": "tuned automatically",
		"write_buffer_limit": "managed by the maintenance settings",
		"hll_columns": "sketches"
}';
	-- the per-table options of a sift_table that 6.0 documents no more
	tbl_removed CONSTANT text[] := ARRAY['wide_rows', 'inline_promote', 'async_promote', 'hot_update', 'bulk_groups', 'dict_lookup', 'key_table', 'auto_defrag_free_pct', 'auto_defrag_budget', 'auto_defrag_coalesce', 'auto_defrag_indexes', 'write_buffer_limit', 'backpressure'];
	tgt text := coalesce(regexp_replace(target, '^([0-9]+\.[0-9]+).*$', '\1'), '6.0');
	ext text;
	lib text;
	avail text;
	r record;
	o record;
	maj int;
	mnr int;
	reload boolean := false;
	preload text;
BEGIN
	IF target IS NOT NULL AND target !~ '^[0-9]+\.[0-9]+' THEN
		RAISE EXCEPTION 'upgrade_check: target must be a release such as 5.1 or 6.0, not "%"', target USING ERRCODE = 'invalid_parameter_value';
	END IF;
	lib := regexp_replace(sift.version(), '^([0-9]+\.[0-9]+).*$', '\1');
	SELECT regexp_replace(e.extversion, '^([0-9]+\.[0-9]+).*$', '\1') INTO ext FROM pg_extension e WHERE e.extname = 'pg_sift';
	SELECT a.default_version INTO avail FROM pg_available_extensions a WHERE a.name = 'pg_sift';

	-- 1. version
	ord := 1; phase := 'upgrade_ext'; object := 'pg_sift'; kind := 'version'; detected := format('extension %s, library %s', ext, lib);
	action_sql := NULL; size_bytes := NULL;
	IF string_to_array(lib, '.')::int[] < string_to_array(ext, '.')::int[] THEN
		status := 'blocker'; advice := format('the library is older than the extension scripts: install pg_sift %s or later', ext);
	ELSIF string_to_array(ext, '.')::int[] < string_to_array(tgt, '.')::int[] THEN
		status := 'warn'; advice := format('ALTER EXTENSION pg_sift UPDATE TO ''%s'' in every database, after the files of the release are installed (%s is available)', tgt, coalesce(avail, 'nothing'));
		action_sql := format('ALTER EXTENSION pg_sift UPDATE TO ''%s'';', tgt);
	ELSE
		status := 'ok'; advice := NULL;
	END IF;
	RETURN NEXT;

	-- 2. setting: the server (file, command line), ALTER DATABASE and ALTER ROLE; the removed names and the -1 of sift.backend_cache_kb that 6.0 does not take
	FOR r IN SELECT x.name, x.value, x.scope, x.cmd
			 FROM (SELECT s.name, s.setting AS value, 'server (' || s.source || ')' AS scope,
						  CASE WHEN s.sourcefile LIKE '%postgresql.auto.conf' THEN format('ALTER SYSTEM RESET %s;', s.name) END AS cmd
				   FROM pg_settings s WHERE s.name LIKE 'sift.%' AND s.source NOT IN ('default', 'override', 'database', 'user', 'database user', 'session', 'client')
				   UNION ALL
				   SELECT f.name, f.setting, 'file ' || f.sourcefile || ':' || f.sourceline,
						  CASE WHEN f.sourcefile LIKE '%postgresql.auto.conf' THEN format('ALTER SYSTEM RESET %s;', f.name) END
				   FROM sift_internal.file_settings() f WHERE NOT EXISTS (SELECT 1 FROM pg_settings p WHERE p.name = f.name)
				   UNION ALL
				   SELECT split_part(c, '=', 1), substr(c, strpos(c, '=') + 1),
						  concat_ws(' ', CASE WHEN d.setrole <> 0 THEN 'ALTER ROLE ' || quote_ident(ro.rolname) END, CASE WHEN d.setdatabase <> 0 THEN 'IN DATABASE ' || quote_ident(db.datname) END),
						  'ALTER ' || CASE WHEN d.setrole <> 0 THEN 'ROLE ' || quote_ident(ro.rolname) || CASE WHEN d.setdatabase <> 0 THEN ' IN DATABASE ' || quote_ident(db.datname) ELSE '' END
									   WHEN d.setdatabase <> 0 THEN 'DATABASE ' || quote_ident(db.datname) ELSE 'ROLE ALL' END || ' RESET ' || split_part(c, '=', 1) || ';'
				   FROM pg_db_role_setting d LEFT JOIN pg_roles ro ON ro.oid = d.setrole LEFT JOIN pg_database db ON db.oid = d.setdatabase, unnest(d.setconfig) c
				   WHERE c LIKE 'sift.%') x
			 WHERE removed ? x.name OR x.name = ANY (test_only) OR x.name LIKE 'sift.test\_%' OR (x.name = 'sift.backend_cache_kb' AND x.value = '-1')
			 ORDER BY x.name, x.scope
	LOOP
		ord := 2; phase := 'pre_guc'; object := r.name; detected := format('%s = %s, set by %s', r.name, r.value, r.scope); status := 'blocker'; size_bytes := NULL;
		kind := CASE WHEN r.name = ANY (test_only) OR r.name LIKE 'sift.test\_%' THEN 'test' ELSE 'setting' END;
		advice := CASE WHEN r.name = 'sift.backend_cache_kb' THEN 'pg_sift 6.0 removes -1 (unlimited) for sift.backend_cache_kb: set a cap in kB (16384 is the default) or RESET it'
					   WHEN kind = 'test' THEN 'a setting of the test build only: remove it'
					   ELSE format('pg_sift 6.0 removes this setting (%s); PostgreSQL refuses a SET of it and ignores it in the configuration with a warning', removed->>r.name) END;
		action_sql := r.cmd;
		IF r.cmd IS NULL THEN advice := advice || '; remove it from ' || r.scope || ' and run SELECT pg_reload_conf()'; END IF;
		reload := reload OR r.scope LIKE 'server%' OR r.scope LIKE 'file%';
		RETURN NEXT;
	END LOOP;
	IF reload THEN
		ord := 2; phase := 'pre_guc'; object := 'pg_reload_conf'; kind := 'setting'; detected := 'a server setting was found above'; status := 'action';
		advice := 'reload the configuration once the settings are reset'; action_sql := 'SELECT pg_reload_conf();'; size_bytes := NULL;
		RETURN NEXT;
	END IF;

	-- 3. option: what 5.1 stored on the indexes and tables and 6.0 no longer documents; it is still read, so these are info
	FOR r IN SELECT c.oid::regclass::text AS rel, a.amname::text AS am, c.reloptions AS opts, pg_relation_size(c.oid) AS bytes
			 FROM pg_class c JOIN pg_am a ON a.oid = c.relam
			 WHERE a.amname IN ('sift', 'sift_col') AND c.relkind = 'i' ORDER BY 4, 1
	LOOP
		FOR o IN SELECT split_part(x, '=', 1) AS name, substr(x, strpos(x, '=') + 1) AS value FROM unnest(r.opts) x
		LOOP
			ord := 3; phase := 'post_cleanup'; object := r.rel; kind := 'option'; status := 'info'; action_sql := NULL; size_bytes := r.bytes;
			IF r.am = 'sift_col' AND col_removed ? o.name THEN
				detected := o.name;
				advice := format('deprecated, still read as before (Format 2.0: %s); optional cleanup', col_removed->>o.name);
				action_sql := format('ALTER INDEX %s RESET (%s);', r.rel, o.name);
				RETURN NEXT;
			ELSIF r.am = 'sift_col' AND o.name = 'scope' AND o.value ~* '\m(cols|max_multiplicity)\s*\(' THEN
				detected := 'scope with cols(...) or max_multiplicity(...)';
				advice := 'deprecated, still read as before (Format 2.0 ignores both: a scope has no fan-out); optional: remove them from the scope option';
				RETURN NEXT;
			ELSIF r.am = 'sift' AND o.name = 'mutable_limit' THEN
				detected := o.name; advice := 'the deprecated name of write_buffer_limit, still read; optional cleanup';
				action_sql := format('ALTER INDEX %s RESET (mutable_limit), SET (write_buffer_limit = %s);', r.rel, o.value);
				RETURN NEXT;
			ELSIF r.am = 'sift' AND o.name = 'build_segments' THEN
				detected := o.name; advice := 'a test-only option, ignored; optional cleanup';
				action_sql := format('ALTER INDEX %s RESET (build_segments);', r.rel);
				RETURN NEXT;
			END IF;
		END LOOP;
	END LOOP;
	-- the rows of sift.table_option_values are for the roles that may read the table (superusers): any other role is told nothing of them, not refused
	IF has_table_privilege(to_regclass('sift.table_option_values'), 'SELECT') THEN
		FOR r IN SELECT c.oid::regclass::text AS rel, v.name, pg_relation_size(c.oid) AS bytes
				 FROM sift.table_option_values v JOIN pg_class c ON c.oid = v.rel::oid WHERE v.name = ANY (tbl_removed) ORDER BY 3, 1, 2
		LOOP
			ord := 3; phase := 'post_cleanup'; object := r.rel; kind := 'option'; detected := r.name; status := 'info'; size_bytes := r.bytes;
			advice := 'a per-table option that Format 2.0 tunes by itself; a Format 1 table keeps using it. Optional cleanup';
			action_sql := format('SELECT sift.table_reset_option(%L, %L);', r.rel, r.name);
			RETURN NEXT;
		END LOOP;
	END IF;

	-- 4. index and table: the format of what is written
	FOR r IN SELECT c.oid AS relid, c.oid::regclass::text AS rel, a.amname::text AS am, pg_relation_size(c.oid) AS bytes, coalesce(i.indisvalid AND i.indisready, true) AS is_valid
			 FROM pg_class c JOIN pg_am a ON a.oid = c.relam LEFT JOIN pg_index i ON i.indexrelid = c.oid
			 WHERE a.amname IN ('sift', 'sift_col', 'sift_table') AND c.relkind IN ('i', 'r', 'm') ORDER BY 4, 2
	LOOP
		ord := 4; phase := NULL; object := r.rel; kind := CASE r.am WHEN 'sift_table' THEN 'table' ELSE 'index' END; action_sql := NULL; size_bytes := r.bytes; advice := NULL;
		IF NOT r.is_valid THEN
			detected := 'invalid index'; status := 'warn'; phase := 'pre_dep';
			advice := 'a concurrent build that failed or is still running; drop it and build it again';
			action_sql := format('DROP INDEX CONCURRENTLY %s;', r.rel);
		ELSIF r.bytes = 0 THEN
			detected := 'nothing written yet'; status := 'ok';
		ELSE
			BEGIN
				SELECT f.major, f.minor INTO maj, mnr FROM sift.index_format(r.relid) f;
			EXCEPTION WHEN OTHERS THEN
				GET STACKED DIAGNOSTICS advice = PG_EXCEPTION_HINT;
				detected := 'unreadable'; status := 'blocker'; advice := SQLERRM || coalesce('. ' || advice, '');
				RETURN NEXT;
				CONTINUE;
			END;
			detected := format('format %s.%s', maj, mnr); status := 'ok';
			IF r.am = 'sift' THEN
				advice := 'read and written as before: no downtime and no REINDEX';
			ELSIF maj >= 2 THEN
				IF string_to_array(tgt, '.')::int[] < ARRAY[6, 0] THEN
					status := 'blocker'; phase := 'pre_dep';
					advice := format('%s cannot read Format 2.0: %s', tgt, CASE r.am WHEN 'sift_col' THEN 'drop the index and build it again with that release'
										   ELSE format('ALTER TABLE %s SET ACCESS METHOD heap before going back', r.rel) END);
					action_sql := CASE r.am WHEN 'sift_table' THEN format('ALTER TABLE %s SET ACCESS METHOD heap;', r.rel) END;
				END IF;
			ELSIF r.am = 'sift_col' THEN
				detected := detected || ' (Format Major 1)'; status := 'info'; phase := 'post_optional_upgrade';
				advice := 'Format 1.x (fully supported; optional REINDEX INDEX CONCURRENTLY upgrades to Format 2.0)';
				action_sql := format('REINDEX INDEX CONCURRENTLY %s;', r.rel);
			ELSE
				detected := detected || ' (Format Major 1)'; status := 'info'; phase := 'post_optional_upgrade';
				advice := 'Format 1.x (fully supported; optional upgrade to Format 2.0 under an ACCESS EXCLUSIVE lock, as VACUUM FULL)';
				action_sql := format('SELECT * FROM sift.upgrade_table(%L);', r.rel);
			END IF;
		END IF;
		RETURN NEXT;
	END LOOP;

	-- 4b. a shared value store of a column (DICT_REF, 5.0 and 5.1): 6.0 reads it as before, and a 5.1 library can still name the indexes that use it; an optional REINDEX under 6.0 makes them Format 2.0
	IF string_to_array(lib, '.')::int[] < ARRAY[6, 0]
	   AND EXISTS (SELECT 1 FROM pg_proc p JOIN pg_language l ON l.oid = p.prolang WHERE p.oid = to_regprocedure('sift.col_value_store_members()') AND l.lanname = 'c') THEN
		FOR r IN EXECUTE 'SELECT m.index_name::text AS rel, pg_relation_size(m.index_name) AS bytes FROM sift.col_value_store_members() m WHERE m.state = ''shared'' ORDER BY 2, 1'
		LOOP
			ord := 4; phase := 'post_optional_upgrade'; object := r.rel; kind := 'index'; detected := 'shared column value store (DICT_REF)'; status := 'info'; size_bytes := r.bytes;
			advice := 'pg_sift 6.0 reads the shared column value store as before; optional REINDEX INDEX CONCURRENTLY under 6.0 rebuilds the index into Format 2.0';
			action_sql := format('REINDEX INDEX CONCURRENTLY %s;', r.rel);
			RETURN NEXT;
		END LOOP;
	END IF;

	-- 5. dependency: what 6.0 replaces by dropping it is the result of explain_why and refusal_slugs (each gains the action column); a view or a function that depends on one stops the update
	FOR r IN SELECT DISTINCT CASE d.classid WHEN 'pg_rewrite'::regclass THEN (SELECT w.ev_class::regclass::text FROM pg_rewrite w WHERE w.oid = d.objid)
											  WHEN 'pg_proc'::regclass THEN d.objid::regprocedure::text
											  ELSE pg_describe_object(d.classid, d.objid, d.objsubid) END AS dep, p.oid::regprocedure::text AS used
			 FROM pg_depend d JOIN pg_proc p ON p.oid = d.refobjid AND d.refclassid = 'pg_proc'::regclass
			 WHERE p.pronamespace = 'sift'::regnamespace AND p.proname IN ('explain_why', 'refusal_slugs') AND d.deptype = 'n'
			   AND NOT EXISTS (SELECT 1 FROM pg_depend e WHERE e.classid = d.classid AND e.objid = d.objid AND e.deptype = 'e')
			 ORDER BY 1, 2
	LOOP
		ord := 5; phase := 'pre_dep'; object := r.dep; kind := 'dependency'; detected := 'depends on ' || r.used; status := 'blocker'; action_sql := NULL; size_bytes := NULL;
		advice := format('6.0 recreates %s with one more column (action), which a dependent object stops: drop %s before ALTER EXTENSION pg_sift UPDATE and create it again after', r.used, r.dep);
		RETURN NEXT;
	END LOOP;
	FOR r IN SELECT p.oid::regprocedure::text AS fn
			 FROM pg_proc p
			 WHERE p.pronamespace NOT IN ('pg_catalog'::regnamespace, 'information_schema'::regnamespace, 'sift'::regnamespace, 'sift_internal'::regnamespace)
			   AND p.prolang IN (SELECT l.oid FROM pg_language l WHERE l.lanname IN ('sql', 'plpgsql')) AND p.prosrc ~* '\msift\.(explain_why|refusal_slugs)\M'
			   AND NOT EXISTS (SELECT 1 FROM pg_depend e WHERE e.classid = 'pg_proc'::regclass AND e.objid = p.oid AND e.deptype = 'e')
			 ORDER BY 1
	LOOP
		ord := 5; phase := 'pre_dep'; object := r.fn; kind := 'dependency'; detected := 'its body names sift.explain_why or sift.refusal_slugs'; status := 'warn'; action_sql := NULL; size_bytes := NULL;
		advice := 'PostgreSQL does not track a name inside a function body: the result of these two gains a column (action), so a caller that lists its columns by position needs a look';
		RETURN NEXT;
	END LOOP;

	-- 6. replication: a published sift_table needs the library at start-up, for the WAL it decodes (a role that may not read shared_preload_libraries, NULL here, is told nothing)
	preload := (SELECT s.setting FROM pg_settings s WHERE s.name = 'shared_preload_libraries');
	IF preload IS NOT NULL AND NOT EXISTS (SELECT 1 FROM unnest(string_to_array(preload, ',')) l
				   WHERE regexp_replace(regexp_replace(btrim(l, E' \t"'), '^.*/', ''), '[.](so|dylib|dll)$', '') = 'pg_sift') THEN
		FOR r IN SELECT format('%I.%I', t.schemaname, t.tablename) AS rel
				 FROM pg_publication_tables t
				 JOIN pg_class c ON c.oid = format('%I.%I', t.schemaname, t.tablename)::regclass
				 JOIN pg_am a ON a.oid = c.relam AND a.amname = 'sift_table'
				 GROUP BY 1 ORDER BY 1
		LOOP
			ord := 6; phase := 'pre_guc'; object := r.rel; kind := 'replication'; detected := 'published, and pg_sift is not in shared_preload_libraries'; status := 'blocker';
			advice := 'add pg_sift to shared_preload_libraries and restart: the WAL of a sift_table is decoded by the library'; action_sql := NULL; size_bytes := NULL;
			RETURN NEXT;
		END LOOP;
	END IF;

	-- 7. test
	FOR r IN SELECT e.extname FROM pg_extension e WHERE e.extname = 'pg_sift_test' LOOP
		ord := 7; phase := 'pre_dep'; object := r.extname; kind := 'test'; detected := 'extension installed'; status := 'warn'; size_bytes := NULL;
		advice := 'it needs the test build of the library and does not load in a release build';
		action_sql := 'DROP EXTENSION pg_sift_test;';
		RETURN NEXT;
	END LOOP;
END
$fn$;
COMMENT ON FUNCTION sift_internal.upgrade_findings(text) IS 'Internal: the findings of sift.upgrade_check with the phase of the runbook and the statement of each (the support of sift.upgrade_check and sift.migrate_plan)';

-- sift.upgrade_check(target): what to look at before ALTER EXTENSION pg_sift UPDATE, a library swap or a downgrade; one row per finding (object, kind, detected, status, advice).
-- status: blocker, warn, info or ok. target, a release such as 5.1, asks what that release could not read: a relation in Format 2.0 is a blocker there. NULL is 6.0.
CREATE OR REPLACE FUNCTION sift.upgrade_check(target text DEFAULT NULL)
RETURNS TABLE (object text, kind text, detected text, status text, advice text)
	LANGUAGE sql STABLE SECURITY INVOKER SET search_path = pg_catalog, sift AS $$
	SELECT f.object, f.kind, f.detected, f.status, f.advice FROM sift_internal.upgrade_findings(target) f ORDER BY f.ord, f.object
$$;
COMMENT ON FUNCTION sift.upgrade_check(text) IS 'What to look at before ALTER EXTENSION pg_sift UPDATE TO 6.0, a library swap or a downgrade, one row per finding (object, kind, detected, status blocker, warn, info or ok, advice). A relation of Format 1 and the options of 5.1 are info: 6.0 reads and writes them. A removed setting is a blocker. A target release such as 5.1 asks what that release could not read.';

-- sift.migrate_plan(target): the findings of sift.upgrade_check as one ordered runbook of statements, for a database with many indexes. Nothing is required before the update but what is
-- a blocker (a removed setting, a dependent view); a relation of Format 1 is an optional step after it, smallest first, so that a quick one shows progress early.
-- phase: pre_guc, pre_dep, upgrade_ext, post_optional_upgrade, post_cleanup, post_verify. severity: blocker, warn, action (do it), info (optional) or ok. action_sql is NULL for a manual edit.
--   psql -At -c "SELECT action_sql FROM sift.migrate_plan() WHERE phase LIKE 'pre\_%' AND action_sql IS NOT NULL ORDER BY step"
CREATE OR REPLACE FUNCTION sift.migrate_plan(target text DEFAULT '6.0')
RETURNS TABLE (step int4, phase text, object text, kind text, severity text, reason text, action_sql text)
	LANGUAGE plpgsql STABLE SECURITY INVOKER SET search_path = pg_catalog, sift AS $fn$
BEGIN
	IF target !~ '^6\.[0-9]+' THEN
		RAISE EXCEPTION 'migrate_plan: the plan is for 6.0, not "%"', target USING ERRCODE = 'invalid_parameter_value', HINT = 'sift.upgrade_check(target) tells what an older release cannot read';
	END IF;
	RETURN QUERY
	WITH plan AS (
		SELECT CASE f.phase WHEN 'pre_guc' THEN 1 WHEN 'pre_dep' THEN 2 WHEN 'upgrade_ext' THEN 3 WHEN 'post_optional_upgrade' THEN 4 WHEN 'post_cleanup' THEN 5 END AS rank,
			   f.phase, f.object, f.kind, f.status AS severity,
			   coalesce(f.advice, f.detected) AS reason, f.action_sql, f.size_bytes
		FROM sift_internal.upgrade_findings(target) f
		WHERE f.phase IS NOT NULL AND (f.action_sql IS NOT NULL OR f.status IN ('blocker', 'warn'))
		UNION ALL
		SELECT 6, 'post_verify', 'sift.check', 'extension', 'action', 'no row: every index and table is sound', 'SELECT * FROM sift.check();', NULL
		UNION ALL
		SELECT 6, 'post_verify', 'sift.health', 'extension', 'action', 'no row: nothing is left to do (a Format 1 relation is listed as info until it is upgraded)', 'SELECT * FROM sift.health();', NULL
	)
	SELECT (row_number() OVER (ORDER BY p.rank, p.action_sql = 'SELECT pg_reload_conf();', p.size_bytes NULLS LAST, p.object, p.action_sql))::int4, p.phase, p.object, p.kind, p.severity, p.reason, p.action_sql
	FROM plan p ORDER BY 1;
END
$fn$;
COMMENT ON FUNCTION sift.migrate_plan(text) IS 'The runbook of the update to 6.0 in order, one statement a row: reset the removed settings (pre_guc), the views that depend on a changed function (pre_dep), ALTER EXTENSION pg_sift UPDATE (upgrade_ext), the optional upgrade of each Format 1 relation (post_optional_upgrade), cleanup (post_cleanup) and checks (post_verify). Nothing runs: action_sql is NULL where a file is edited by hand.';
