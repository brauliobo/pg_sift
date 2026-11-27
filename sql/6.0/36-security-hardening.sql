-- Security hardening of the objects that 5.0 and 5.1 made: no role runs its own code with the rights of the owner of the extension, and no role reads or locks what it may not touch.

-- Whether the role that called the outermost function may look at a table (sift._may_inspect for the caller of a SECURITY DEFINER function, which sift._may_inspect cannot see) and the rights
-- the pairs of a followed table are refreshed with: the refresh of a statement trigger runs with the rights of the owner of the pair table, never those of the owner of the extension.
CREATE OR REPLACE FUNCTION sift._may_inspect_outer(rel regclass) RETURNS boolean
	AS 'MODULE_PATHNAME', 'sift_may_inspect_outer' LANGUAGE C STABLE PARALLEL SAFE;
CREATE OR REPLACE FUNCTION sift._in_follow_refresh() RETURNS boolean
	AS 'MODULE_PATHNAME', 'sift_in_follow_refresh' LANGUAGE C STABLE PARALLEL RESTRICTED;
CREATE OR REPLACE FUNCTION sift._scope_refresh_as_owner(f sift.scope_follows, ids bigint[], keys bigint[]) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_scope_refresh_as_owner' LANGUAGE C VOLATILE;
REVOKE EXECUTE ON FUNCTION sift._scope_refresh_as_owner(sift.scope_follows, bigint[], bigint[]) FROM PUBLIC;
COMMENT ON FUNCTION sift._may_inspect_outer(regclass) IS 'Internal: whether the role that called the outermost function may look at the sift objects of the table (sift._may_inspect for a SECURITY DEFINER function)';
COMMENT ON FUNCTION sift._in_follow_refresh() IS 'Internal: whether the refresh of the pairs of a followed table runs, with the rights of the owner of the pairs';
COMMENT ON FUNCTION sift._scope_refresh_as_owner(sift.scope_follows, bigint[], bigint[]) IS 'Internal: sift._scope_refresh_for with the rights of the owner of the pair table';

CREATE OR REPLACE FUNCTION sift._require_inspect_outer(rel regclass) RETURNS void LANGUAGE plpgsql STABLE SET search_path = pg_catalog, pg_temp AS $$
BEGIN
	IF NOT sift._may_inspect_outer(rel) THEN
		RAISE EXCEPTION 'permission denied for table %', COALESCE(rel::text, '(none)') USING ERRCODE = 'insufficient_privilege';
	END IF;
END $$;
COMMENT ON FUNCTION sift._require_inspect_outer(regclass) IS 'Internal: permission denied unless the role that called the outermost function may look at the sift objects of the table';

-- The pairs of a followed table: the functions of a role run as that role (the view, the triggers, the checks and the indexes of its tables are its own code), and only the writes of
-- sift.scope_follows, which no role may touch, are made as the owner of the extension, by the helpers below. The guard trigger of the catalog asks the role that called the outermost function.
CREATE OR REPLACE FUNCTION sift._follow_put(f sift.scope_follows) RETURNS void LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
BEGIN
	INSERT INTO sift.scope_follows SELECT f.*;
END $$;
CREATE OR REPLACE FUNCTION sift._follow_drop(pairs regclass, source regclass) RETURNS SETOF sift.scope_follows
	LANGUAGE sql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
	DELETE FROM sift.scope_follows s WHERE s.pairs = $1 AND ($2 IS NULL OR s.source = $2) RETURNING s.*
$$;
CREATE OR REPLACE FUNCTION sift._follows_expiring(pairs regclass) RETURNS SETOF sift.scope_follows
	LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
BEGIN
	PERFORM sift._require_owner(pairs);
	RETURN QUERY SELECT s.* FROM sift.scope_follows s WHERE s.pairs = $1 AND s.expires IS NOT NULL ORDER BY s.source FOR UPDATE;
END $$;
CREATE OR REPLACE FUNCTION sift._follow_swept(f sift.scope_follows) RETURNS void LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
BEGIN
	UPDATE sift.scope_follows s SET swept_at = now() WHERE s.pairs = f.pairs AND s.source = f.source AND s.source_column = f.source_column;
END $$;
COMMENT ON FUNCTION sift._follow_put(sift.scope_follows) IS 'Internal: writes a row of sift.scope_follows as the owner of the extension; the guard trigger checks the caller owns the tables it names';
COMMENT ON FUNCTION sift._follow_drop(regclass, regclass) IS 'Internal: deletes the rows of sift.scope_follows of a pair table (and one source) as the owner of the extension; the guard trigger checks the caller owns the tables';
COMMENT ON FUNCTION sift._follows_expiring(regclass) IS 'Internal: the locked rows of the sources of a pair table that have an expires column, for the owner of the pairs';
COMMENT ON FUNCTION sift._follow_swept(sift.scope_follows) IS 'Internal: records the sweep of a followed table as the owner of the extension; the guard trigger checks the caller owns the tables';

-- What a change of the rows of a followed table refreshes: everything (a TRUNCATE) or what the rows name
CREATE OR REPLACE FUNCTION sift._scope_refresh_for(f sift.scope_follows, ids bigint[], keys bigint[]) RETURNS void LANGUAGE plpgsql SET search_path = pg_catalog, pg_temp AS $$
BEGIN
	IF ids IS NULL THEN
		PERFORM sift.scope_refresh_pairs(f.pairs, f.definition, f.refresh_by);
	ELSE
		PERFORM sift.scope_refresh_named(f, ids, keys);
	END IF;
END $$;
COMMENT ON FUNCTION sift._scope_refresh_for(sift.scope_follows, bigint[], bigint[]) IS 'Internal: refreshes all the pairs of a followed table (ids NULL) or what scope_names_sql named';

CREATE OR REPLACE FUNCTION sift._pair_columns(pairs regclass, definition regclass) RETURNS name[] LANGUAGE plpgsql STABLE SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
BEGIN
	IF NOT (sift._may_inspect_outer(pairs) OR sift._in_follow_refresh()) THEN
		RAISE EXCEPTION 'permission denied for table %', pairs::text USING ERRCODE = 'insufficient_privilege';
	END IF;
	RETURN COALESCE((SELECT ARRAY[f.member_column, f.key_column] FROM sift.scope_follows f WHERE f.pairs = $1 AND f.definition = $2 ORDER BY f.source LIMIT 1),
					(SELECT array_agg(a.attname ORDER BY a.attnum) FROM pg_attribute a WHERE a.attrelid = $2 AND a.attnum > 0 AND NOT a.attisdropped));
END $$;

-- The statement trigger of a followed table fires for every role that writes the table: it reads sift.scope_follows and refreshes the pairs with the rights of their owner.
-- new_rows and old_rows name its transition tables only when the trigger declares them; otherwise they would name a relation of pg_temp, which the writer defines.
CREATE OR REPLACE FUNCTION sift.scope_follow_trg() RETURNS trigger LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
DECLARE
	f    sift.scope_follows;
	ids  bigint[];
	keys bigint[];
BEGIN
	SELECT * INTO STRICT f FROM sift.scope_follows s WHERE s.source::oid = TG_RELID AND s.trigger_base = regexp_replace(TG_NAME, '_s(ins|upd|del|t)$', '');
	IF TG_OP <> 'TRUNCATE' AND NOT EXISTS (SELECT 1 FROM pg_trigger t WHERE t.tgrelid = TG_RELID AND t.tgname = TG_NAME
										   AND t.tgoldtable IS NOT DISTINCT FROM CASE WHEN TG_OP <> 'INSERT' THEN 'old_rows' END
										   AND t.tgnewtable IS NOT DISTINCT FROM CASE WHEN TG_OP <> 'DELETE' THEN 'new_rows' END) THEN
		RAISE EXCEPTION 'scope_follow_trg: trigger "%" of "%" does not declare the transition tables of a scope follow', TG_NAME, f.source::text
			USING HINT = 'Follow the table again with sift.scope_follow_pairs.', ERRCODE = 'object_not_in_prerequisite_state';
	END IF;
	IF TG_OP <> 'TRUNCATE' THEN
		IF (SELECT count(*) FROM pg_attribute a WHERE a.attrelid = f.source AND a.attname IN (f.source_column, f.source_member) AND NOT a.attisdropped
			AND a.atttypid NOT IN ('int2'::regtype, 'int4'::regtype, 'int8'::regtype)) > 0 THEN
			RAISE EXCEPTION 'scope_follow_trg: a followed column of "%" is not an integer column any more', f.source::text USING ERRCODE = 'datatype_mismatch';
		END IF;
		EXECUTE sift.scope_names_sql(f, CASE TG_OP WHEN 'INSERT' THEN 'SELECT * FROM new_rows' WHEN 'DELETE' THEN 'SELECT * FROM old_rows'
											ELSE 'SELECT * FROM old_rows UNION ALL SELECT * FROM new_rows' END) INTO ids, keys;
	END IF;
	PERFORM sift._scope_refresh_as_owner(f, ids, keys);
	RETURN NULL;
END $$;

CREATE OR REPLACE FUNCTION sift.scope_follow_pairs(pairs regclass, definition regclass, source regclass, by text, source_column name, expires name DEFAULT NULL)
	RETURNS TABLE (inserted bigint, deleted bigint) LANGUAGE plpgsql SECURITY INVOKER SET search_path = pg_catalog, pg_temp AS $$
DECLARE
	src    text := sift.scope_qualified(source);
	base   name := format('sift_scope_%s', left(md5(format('%s %s %s', pairs::oid, source::oid, source_column)), 12));
	tr     text;
BEGIN
	IF (SELECT relkind FROM pg_class WHERE oid = source) NOT IN ('r', 'p') THEN
		RAISE EXCEPTION 'scope_follow_pairs: "%" is not a table', src USING ERRCODE = 'wrong_object_type';
	END IF;
	IF by NOT IN ('member', 'key', 'pair') THEN
		RAISE EXCEPTION 'scope_follow_pairs: by is "%", it must be ''member'', ''key'' or ''pair''', by USING ERRCODE = 'invalid_parameter_value';
	END IF;
	IF by = 'pair' AND (SELECT count(*) FROM pg_attribute a, pg_attribute v WHERE a.attrelid = source AND NOT a.attisdropped AND v.attrelid = definition AND v.attnum IN (1, 2)
						AND v.attname = a.attname AND a.atttypid IN ('int2'::regtype, 'int4'::regtype, 'int8'::regtype)) <> 2 THEN
		RAISE EXCEPTION 'scope_follow_pairs: by pair needs the two columns of the view as integer columns of "%"', src USING ERRCODE = 'undefined_column';
	END IF;
	IF by = 'pair' AND source_column <> (SELECT attname FROM pg_attribute WHERE attrelid = definition AND attnum = 2) THEN
		RAISE EXCEPTION 'scope_follow_pairs: by pair takes the scope key column of the view as source_column' USING ERRCODE = 'invalid_parameter_value';
	END IF;
	IF NOT EXISTS (SELECT 1 FROM pg_attribute a WHERE a.attrelid = source AND a.attname = source_column AND NOT a.attisdropped
				   AND a.atttypid IN ('int2'::regtype, 'int4'::regtype, 'int8'::regtype)) THEN
		RAISE EXCEPTION 'scope_follow_pairs: table % has no integer column "%"', src, source_column USING ERRCODE = 'undefined_column';
	END IF;
	IF expires IS NOT NULL AND NOT EXISTS (SELECT 1 FROM pg_attribute a WHERE a.attrelid = source AND a.attname = expires AND NOT a.attisdropped
										   AND a.atttypid IN ('date'::regtype, 'timestamp'::regtype, 'timestamptz'::regtype)) THEN
		RAISE EXCEPTION 'scope_follow_pairs: table % has no date or timestamp column "%"', src, expires USING ERRCODE = 'undefined_column';
	END IF;
	IF EXISTS (SELECT 1 FROM pg_subscription_rel r WHERE r.srrelid IN (source, pairs)) THEN
		RAISE EXCEPTION 'scope_follow_pairs: a table is the target of a logical replication subscription'
			USING DETAIL = 'The apply worker changes rows without firing statement triggers, so the pairs would miss the changes.', ERRCODE = 'feature_not_supported';
	END IF;
	PERFORM sift._follow_put(ROW(pairs, definition, source, base, by, source_column, CASE WHEN by = 'pair' THEN v.m END, v.m, v.k, expires, now())::sift.scope_follows)
		FROM (SELECT (array_agg(a.attname ORDER BY a.attnum))[1] m, (array_agg(a.attname ORDER BY a.attnum))[2] k FROM pg_attribute a
			  WHERE a.attrelid = definition AND a.attnum > 0 AND NOT a.attisdropped) v;
	FOR tr IN SELECT unnest(ARRAY['ins', 'upd', 'del']) LOOP
		EXECUTE format('CREATE TRIGGER %I AFTER %s ON %s REFERENCING %s AS %s FOR EACH STATEMENT EXECUTE FUNCTION sift.scope_follow_trg()',
					   base || '_s' || tr, CASE tr WHEN 'ins' THEN 'INSERT' WHEN 'upd' THEN 'UPDATE' ELSE 'DELETE' END, src,
					   CASE tr WHEN 'ins' THEN 'NEW TABLE' WHEN 'upd' THEN 'OLD TABLE AS old_rows NEW TABLE' ELSE 'OLD TABLE' END,
					   CASE tr WHEN 'del' THEN 'old_rows' ELSE 'new_rows' END);
	END LOOP;
	EXECUTE format('CREATE TRIGGER %I AFTER TRUNCATE ON %s FOR EACH STATEMENT EXECUTE FUNCTION sift.scope_follow_trg()', base || '_st', src);
	-- the triggers hold the source against writers until this transaction ends, so the full refresh below is the state they continue from
	RETURN QUERY SELECT * FROM sift.scope_refresh_pairs(pairs, definition, by);
END $$;

CREATE OR REPLACE FUNCTION sift.scope_unfollow_pairs(pairs regclass, source regclass DEFAULT NULL) RETURNS void LANGUAGE plpgsql SECURITY INVOKER SET search_path = pg_catalog, pg_temp AS $$
DECLARE
	f sift.scope_follows;
	t text;
BEGIN
	FOR f IN SELECT * FROM sift._follow_drop(scope_unfollow_pairs.pairs, scope_unfollow_pairs.source) LOOP
		FOREACH t IN ARRAY ARRAY['sins', 'supd', 'sdel', 'st'] LOOP
			EXECUTE format('DROP TRIGGER IF EXISTS %I ON %s', f.trigger_base || '_' || t, f.source);
		END LOOP;
	END LOOP;
	IF NOT FOUND THEN
		RAISE EXCEPTION 'scope_unfollow_pairs: "%" follows no such table', scope_unfollow_pairs.pairs USING ERRCODE = 'undefined_object';
	END IF;
END $$;

CREATE OR REPLACE FUNCTION sift.scope_expire_pairs(pairs regclass, overlap interval DEFAULT '1 hour') RETURNS TABLE (inserted bigint, deleted bigint) LANGUAGE plpgsql SECURITY INVOKER SET search_path = pg_catalog, pg_temp AS $$
DECLARE
	f      sift.scope_follows;
	ids    bigint[];
	keys   bigint[];
	r      record;
	typ    text;
BEGIN
	inserted := 0; deleted := 0; PERFORM sift._require_owner(scope_expire_pairs.pairs);
	FOR f IN SELECT * FROM sift._follows_expiring(scope_expire_pairs.pairs) LOOP
		PERFORM sift._require_follow(f);
		SELECT format_type(a.atttypid, NULL) INTO typ FROM pg_attribute a WHERE a.attrelid = f.source AND a.attname = f.expires;
		EXECUTE sift.scope_names_sql(f, format('SELECT * FROM %s WHERE %I BETWEEN %L::%s AND %L::%s', f.source, f.expires, f.swept_at - overlap, typ, now(), typ)) INTO ids, keys;
		SELECT * INTO r FROM sift.scope_refresh_named(f, ids, keys);
		inserted := inserted + r.inserted; deleted := deleted + r.deleted;
		PERFORM sift._follow_swept(f);
	END LOOP;
	IF NOT FOUND THEN
		RAISE EXCEPTION 'scope_expire_pairs: no source of "%" has an expires column', scope_expire_pairs.pairs USING ERRCODE = 'undefined_object';
	END IF;
	RETURN NEXT;
END $$;

-- The functions that read the definitions of projections answer for the tables the role that called them may look at, and sift.projection_status runs no function of a role on rows it hides
CREATE OR REPLACE FUNCTION sift.projection_holds(idx oid, pname text) RETURNS text LANGUAGE plpgsql STABLE SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
DECLARE
	p     sift.projections;
	ix    pg_index;
	have  text;
	o     record;
BEGIN
	SELECT * INTO p FROM sift.projections WHERE name = pname;
	IF NOT FOUND THEN RETURN format('the projection "%s" is not defined', pname); END IF;
	PERFORM sift._require_inspect_outer(to_regclass(p.source));
	SELECT * INTO ix FROM pg_index WHERE indexrelid = idx;
	IF ix.indrelid IS DISTINCT FROM to_regclass(p.source)::oid THEN RETURN format('the index is not on %s', p.source); END IF;
	IF NOT (ix.indisvalid AND ix.indisready) THEN RETURN 'the index is not valid'; END IF;
	FOREACH have IN ARRAY p.canonical LOOP
		IF NOT EXISTS (SELECT 1 FROM generate_series(1, ix.indnatts) k WHERE pg_get_indexdef(idx, k, true) = have) THEN
			RETURN format('the index does not hold %s', have);
		END IF;
	END LOOP;
	IF NOT sift.projection_filter_served(ix.indrelid, COALESCE(p.filter, 'true'), idx) THEN
		RETURN format('the index keeps other rows than the filter or lacks its columns (%s)', COALESCE(pg_get_expr(ix.indpred, ix.indrelid, true), 'no predicate'));
	END IF;
	IF NOT sift.projection_rollup_holds(sift.projection_option(idx, 'rollup_columns'), p.rollups) THEN
		RETURN format('the index does not roll up %s', p.rollups);
	END IF;
	FOR o IN SELECT key, value FROM jsonb_each_text(p.options) LOOP
		IF sift.projection_option(idx, o.key) IS DISTINCT FROM o.value THEN
			RETURN format('the option %s of the index is not %s', o.key, o.value);
		END IF;
	END LOOP;
	RETURN NULL;
END $$;

CREATE OR REPLACE FUNCTION sift.projection_serves(idx oid, names text[], layout jsonb) RETURNS text LANGUAGE plpgsql STABLE SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
DECLARE
	n   text;
	why text;
BEGIN
	PERFORM sift._require_inspect_outer((SELECT x.indrelid FROM pg_index x WHERE x.indexrelid = idx));
	IF NOT sift.projection_same_layout(idx, layout) THEN RETURN 'the index has another layout'; END IF;
	FOREACH n IN ARRAY names LOOP
		why := sift.projection_holds(idx, n);
		IF why IS NOT NULL THEN RETURN why; END IF;
	END LOOP;
	RETURN NULL;
END $$;

-- The CREATE INDEX of projections is made of the text of their definitions, which a role stored and a superuser may run: each column is one index element, the filter one expression and
-- the statement one CREATE INDEX of the table. The probe of a definition and the definitions a role writes are held to the same statement.
CREATE OR REPLACE FUNCTION sift._projection_index_sql(index text, source regclass, leaves text[], options text, filter text, conc boolean) RETURNS text
	AS 'MODULE_PATHNAME', 'sift_col_projection_index_sql' LANGUAGE C STABLE;
COMMENT ON FUNCTION sift._projection_index_sql(text, regclass, text[], text, text, boolean) IS 'Internal: the CREATE INDEX of projections, refused unless each column is one index element, the filter one expression and the statement one CREATE INDEX';

CREATE OR REPLACE FUNCTION sift.projection_probe(source text, leaves text[], rollups text, filter text, options jsonb, OUT canonical text[], OUT canonical_filter text)
	LANGUAGE plpgsql SET client_min_messages = warning SET search_path = pg_catalog, pg_temp AS $$
DECLARE
	idx oid;
BEGIN
	EXECUTE format('CREATE TEMPORARY TABLE sift_projection_probe (LIKE %s)', source::regclass);
	EXECUTE sift._projection_index_sql('sift_projection_probe_i', 'pg_temp.sift_projection_probe'::regclass, leaves, sift.projection_with(ARRAY[]::text[], rollups, options, '{}'), filter, false);
	idx := 'pg_temp.sift_projection_probe_i'::regclass;
	canonical := ARRAY(SELECT pg_get_indexdef(idx, k, true) FROM generate_series(1, (SELECT indnatts FROM pg_index WHERE indexrelid = idx)) k ORDER BY k);
	canonical_filter := pg_get_expr((SELECT indpred FROM pg_index WHERE indexrelid = idx), (SELECT indrelid FROM pg_index WHERE indexrelid = idx), true);
	DROP TABLE pg_temp.sift_projection_probe;
END $$;

-- A definition written as the owner of the extension names its table by its qualified name, as projection_create does, and is one CREATE INDEX: a role that calls the helper itself
-- stores no other text
CREATE OR REPLACE FUNCTION sift._projection_put(p sift.projections, replace boolean) RETURNS void
	LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
BEGIN
	IF p.source IS DISTINCT FROM sift.scope_qualified(to_regclass(p.source)) OR (SELECT relpersistence FROM pg_class WHERE oid = to_regclass(p.source)) = 't' THEN
		RAISE EXCEPTION 'a projection names a table by its qualified name, not "%"', p.source USING ERRCODE = 'invalid_parameter_value';
	END IF;
	PERFORM sift._projection_index_sql(p.name, to_regclass(p.source), p.leaves, NULL, p.filter, false);
	IF replace THEN
		UPDATE sift.projections x SET leaves = p.leaves, rollups = p.rollups, filter = p.filter, options = p.options, canonical = p.canonical, canonical_filter = p.canonical_filter
			WHERE x.name = p.name;
	ELSE
		INSERT INTO sift.projections SELECT p.*;
	END IF;
END $$;

CREATE OR REPLACE FUNCTION sift.projection_plan(index text, projections text[], layout jsonb, conc boolean, OUT serving oid, OUT statement text) LANGUAGE plpgsql STABLE SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
DECLARE
	d      sift.projections[];
	p      sift.projections;
	n      text;
	have   text[] := '{}';
	cols   text[] := '{}';
	i      int;
	named  oid;
BEGIN
	IF projections IS NULL OR cardinality(projections) = 0 THEN
		RAISE EXCEPTION 'projection_index: no projection is named' USING ERRCODE = 'invalid_parameter_value';
	END IF;
	FOREACH n IN ARRAY projections LOOP
		SELECT * INTO p FROM sift.projections WHERE name = n;
		IF NOT FOUND THEN
			RAISE EXCEPTION 'projection_index: the projection "%" is not defined', n USING ERRCODE = 'undefined_object';
		END IF;
		PERFORM sift._require_inspect_outer(to_regclass(p.source));
		d := d || p;
	END LOOP;
	IF EXISTS (SELECT 1 FROM unnest(d) x WHERE (x.source, x.filter, x.rollups, x.options) IS DISTINCT FROM (d[1].source, d[1].filter, d[1].rollups, d[1].options)) THEN
		RAISE EXCEPTION 'projection_index: the projections % differ in table, filter, rollups or options', projections USING ERRCODE = 'invalid_parameter_value',
			HINT = 'one index holds projections with the same table, filter, rollups and options: make one index of each kind';
	END IF;
	IF EXISTS (SELECT 1 FROM jsonb_object_keys(layout) k WHERE k IN ('projection', 'rollup_columns')) THEN
		RAISE EXCEPTION 'projection_index: the layout cannot set projection or rollup_columns' USING ERRCODE = 'invalid_parameter_value';
	END IF;
	SELECT i.indexrelid INTO serving FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam AND a.amname = 'sift_col'
		WHERE i.indrelid = to_regclass(d[1].source) AND sift.projection_serves(i.indexrelid, projections, layout) IS NULL
		ORDER BY (c.relname = index) DESC, pg_relation_size(i.indexrelid), c.relname LIMIT 1;
	IF serving IS NOT NULL THEN
		statement := format('ALTER INDEX %s SET (projection = %L)', serving::regclass,
							array_to_string(ARRAY(SELECT DISTINCT u FROM unnest(sift.projection_names(serving) || projections) u ORDER BY u), ', '));
		RETURN;
	END IF;
	IF to_regclass(format('%s.%I', (SELECT nspname FROM pg_namespace WHERE oid = (SELECT relnamespace FROM pg_class WHERE oid = to_regclass(d[1].source))), index)) IS NOT NULL THEN
		RAISE EXCEPTION 'projection_index: relation "%" already exists and does not hold the projections', index USING ERRCODE = 'duplicate_table';
	END IF;
	FOR i IN 1 .. cardinality(d) LOOP
		FOREACH n IN ARRAY d[i].leaves LOOP
			IF NOT (d[i].canonical[array_position(d[i].leaves, n)] = ANY (have)) THEN
				cols := cols || n;
				have := have || d[i].canonical[array_position(d[i].leaves, n)];
			END IF;
		END LOOP;
	END LOOP;
	statement := sift._projection_index_sql(index, to_regclass(d[1].source), cols, sift.projection_with(projections, d[1].rollups, d[1].options, layout), d[1].filter, conc);
END $$;

ALTER VIEW sift.projection_status SET (security_barrier = true);

-- The trigger functions of the catalogs of the extension act on the table a row names: only the triggers of those catalogs run them
REVOKE EXECUTE ON FUNCTION sift.table_paths_trg() FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION sift.table_option_values_trg() FROM PUBLIC;

-- What sift.health and sift.activity say of the shared memory is for the roles that may read it (pg_read_all_stats); for another role the pool rows are not there, as the sift.shared_memory functions refuse it
CREATE OR REPLACE FUNCTION sift_internal.shared_memory_pools_visible(OUT pool text, OUT limit_bytes int8, OUT used_bytes int8, OUT peak_bytes int8, OUT util_pct numeric(5,2), OUT evictions int8,
																	 OUT refusals int8, OUT shared bool, OUT reason text)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_shared_memory_pools_visible' LANGUAGE C STABLE;
COMMENT ON FUNCTION sift_internal.shared_memory_pools_visible() IS 'Internal: the rows of sift.shared_memory_pools() for a member of pg_read_all_stats, none for another role';

CREATE OR REPLACE FUNCTION sift_internal.health_over(statuses jsonb, rel regclass DEFAULT NULL)
RETURNS TABLE (relation regclass, severity text, check_name text, detail text, action_sql text)
	LANGUAGE sql STABLE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $fn$
	WITH cfg AS (
		SELECT coalesce((SELECT setting FROM pg_settings WHERE name = 'sift.async_maintenance'), 'on') = 'off' AS async_off,
			   coalesce((SELECT setting::int8 FROM pg_settings WHERE name = 'sift.shared_cache_kb'), 131072) AS shared_kb
	), st AS MATERIALIZED (
		SELECT s.relation::regclass AS relation, s.table_name::regclass AS table_name, s.am, s.format_major, s.size_bytes, s.quarantined, s.quarantine_reason, s.vblocks_left,
			   s.maint_failures, s.free_bytes, s.shrink_blocked_by, s.dead_pct, s.dead_rows, s.live_rows, s.last_maint_at, s.l0_parts, s.l0_slabs, s.scopes,
			   i.indisvalid AND i.indisready AS is_valid, h.relpages, h.relallvisible, w.error_code, w.last_error, w.next_retry,
			   s.live_rows + s.dead_rows AS total_rows
		FROM jsonb_to_recordset(statuses) AS s(relation oid, table_name oid, am text, format_major int2, size_bytes int8, quarantined bool, quarantine_reason text, vblocks_left int8,
										 maint_failures int4, free_bytes int8, shrink_blocked_by text, dead_pct numeric, dead_rows int8, live_rows int8, last_maint_at timestamptz,
										 l0_parts int4, l0_slabs int4, scopes int2)
		LEFT JOIN pg_index i ON i.indexrelid = s.relation
		LEFT JOIN pg_class h ON h.oid = s.table_name
		LEFT JOIN LATERAL sift.worker_state(CASE WHEN s.maint_failures > 0 THEN s.relation END) w ON true
	), found AS (
		SELECT s.relation, 'info' AS severity, 'format_upgrade_available' AS check_name,
			   'Format 1.x (fully supported; optional REINDEX INDEX CONCURRENTLY upgrades to Format 2.0)' AS detail,
			   format('REINDEX INDEX CONCURRENTLY %s;', s.relation) AS action_sql
		FROM st s WHERE s.am = 'sift_col' AND s.format_major < 2 AND s.size_bytes > 0 AND s.is_valid
		  AND NOT EXISTS (SELECT 1 FROM pg_depend d JOIN pg_class u ON u.oid = d.objid AND u.relkind = 'i'
						  WHERE d.classid = 'pg_class'::regclass AND d.refclassid = 'pg_class'::regclass AND d.refobjid = s.relation::oid AND d.objsubid = 0 AND d.deptype = 'n'
							AND EXISTS (SELECT 1 FROM unnest(u.reloptions) o WHERE o LIKE 'scope=%'))
		UNION ALL
		SELECT s.relation, 'info', 'format_upgrade_available', 'Format 1.x (fully supported; optional upgrade to Format 2.0)',
			   format('SELECT * FROM sift.upgrade_table(%L);', s.relation::text)
		FROM st s WHERE s.am = 'sift_table' AND s.format_major < 2 AND s.size_bytes > 0
		UNION ALL
		SELECT s.relation, 'error', 'index_quarantined', 'Quarantined: ' || coalesce(s.quarantine_reason, 'out of service'),
			   CASE WHEN s.am = 'sift_table' THEN format('SELECT * FROM sift.check(%L);', s.relation::text)
					ELSE format('REINDEX INDEX CONCURRENTLY %s;', s.relation) END
		FROM st s WHERE s.quarantined
		UNION ALL
		SELECT s.relation, 'error', 'index_invalid', 'Interrupted concurrent index build left an invalid index',
			   format('DROP INDEX CONCURRENTLY %s;', s.relation)
		FROM st s WHERE NOT s.is_valid AND NOT EXISTS (SELECT 1 FROM pg_stat_progress_create_index p WHERE p.index_relid = s.relation)
		UNION ALL
		SELECT s.relation, CASE WHEN s.vblocks_left < 85899345 THEN 'error' ELSE 'warn' END, 'vblocks_low',
			   format('Only %s%% of virtual TID blocks remain (%s left)', round(100.0 * s.vblocks_left / 4294967294, 1), s.vblocks_left),
			   format('VACUUM %s;', s.relation)
		FROM st s WHERE s.am = 'sift_table' AND s.vblocks_left < 429496729
		UNION ALL
		SELECT s.relation, CASE WHEN s.maint_failures >= 5 THEN 'error' ELSE 'warn' END, 'worker_failing',
			   format('%s consecutive failures (last: %s, next retry %s)', s.maint_failures, coalesce(nullif(concat_ws(' ', s.error_code, s.last_error), ''), 'no error recorded'),
					  coalesce(s.next_retry::text, 'not scheduled')),
			   format('SELECT * FROM sift.maintain(%L);', s.relation::text)
		FROM st s WHERE s.maint_failures > 0
		UNION ALL
		SELECT s.relation, 'warn', 'table_options_unapplied', 'Parts written before the sort key or the declared paths the table has now (a restore loads the rows before the options and the key)',
			   format('VACUUM FULL %s;', s.relation)
		FROM st s WHERE s.am = 'sift_table' AND s.format_major >= 2 AND NOT s.quarantined AND sift_internal.table_options_unapplied(s.relation)
		UNION ALL
		SELECT s.relation, 'warn', 'tail_shrink_blocked',
			   format('%s (%s%%) free space blocked by %s', pg_size_pretty(s.free_bytes), round(100.0 * s.free_bytes / s.size_bytes, 1), s.shrink_blocked_by),
			   format('SELECT * FROM sift.maintain(%L);', s.relation::text)
		FROM st s WHERE s.free_bytes >= 4194304 AND s.free_bytes * 100 >= s.size_bytes * 15 AND s.shrink_blocked_by IS NOT NULL
		UNION ALL
		SELECT s.relation, 'warn', 'high_dead_rows', format('%s%% dead rows (%s rows)', s.dead_pct, s.dead_rows),
			   CASE WHEN c.async_off THEN 'ALTER SYSTEM SET sift.async_maintenance = on; SELECT pg_reload_conf();'
					ELSE format('SELECT sift.merge(%L);', s.relation::text) END
		FROM st s CROSS JOIN cfg c
		WHERE s.dead_pct >= 25 AND (c.async_off OR (s.total_rows >= 10000 AND coalesce(s.last_maint_at, '-infinity') < now() - interval '10 minutes'))
		UNION ALL
		SELECT s.relation, 'warn', 'l0_backlog', format('Write backlog: %s L0 parts, %s slabs awaiting compaction', coalesce(s.l0_parts, 0), coalesce(s.l0_slabs, 0)),
			   format('SELECT sift.promote(%L);', s.relation::text)
		FROM st s WHERE s.l0_parts >= 12 OR s.l0_slabs >= 32
		UNION ALL
		SELECT s.relation, 'warn', 'stale_scope', format('Scope "%s" has %s/%s dirty keys awaiting refresh', sc.scope_name, sc.dirty_keys, sc.nkeys),
			   format('SELECT * FROM sift.col_refresh(%L);', s.relation::text)
		FROM st s CROSS JOIN LATERAL sift.scope_stats(s.relation) sc
		WHERE s.am = 'sift_col' AND s.scopes > 0 AND sc.nkeys > 0 AND sc.dirty_keys * 100 > sc.nkeys * 10
		UNION ALL
		SELECT s.relation, 'info', 'heap_visibility_low',
			   format('Only %s%% of heap pages all-visible; scans perform extra heap vetting', round(100.0 * s.relallvisible / s.relpages)),
			   format('VACUUM %s;', s.table_name)
		FROM st s WHERE s.am = 'sift_col' AND s.relpages >= 64 AND s.relallvisible::numeric / s.relpages < 0.5
		UNION ALL
		SELECT NULL::regclass, 'info', 'shared_cache_pressure',
			   format('Shared cache (%s) at %s%% utilization (%s refusals)', pg_size_pretty(sum(p.limit_bytes)),
					  round(100.0 * sum(p.used_bytes) / sum(p.limit_bytes)), sum(p.refusals)),
			   format('ALTER SYSTEM SET sift.shared_cache_kb = %L; SELECT pg_reload_conf();', (SELECT shared_kb * 2 FROM cfg) || 'kB')
		FROM sift_internal.shared_memory_pools_visible() p
		WHERE rel IS NULL HAVING sum(p.limit_bytes) > 0 AND (sum(p.refusals) > 0 OR bool_or(NOT p.shared) OR 100.0 * sum(p.used_bytes) / sum(p.limit_bytes) >= 90)
		UNION ALL
		SELECT NULL::regclass, 'info', 'extension_not_preloaded', 'pg_sift is not preloaded; the first statement of each session is planned twice', sift_internal.preload_hint()
		WHERE rel IS NULL AND sift_internal.preload_hint() IS NOT NULL
	)
	SELECT f.relation, f.severity, f.check_name, f.detail, f.action_sql
	FROM found f
$fn$;

CREATE OR REPLACE FUNCTION sift.activity()
RETURNS TABLE (kind text, pid int4, relation regclass, part_id int8, job text, phase text, priority int4, done int8, total int8, progress_pct numeric(5,2),
			   bytes_written int8, elapsed_ms int8, throughput_mb_s numeric(8,2), eta_s numeric(10,1), started_at timestamptz, wait_event text,
			   workers_active int4, workers_max int4, pacer_mode text, pacer_delay_ms int4, oltp_active_backends int4,
			   shm_used_bytes int8, shm_limit_bytes int8, shm_util_pct numeric(5,2), shm_refusals int8)
	LANGUAGE sql STABLE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp AS $fn$
	WITH pc AS (
		SELECT * FROM sift_internal.maint_pacer()
	), shm AS (
		SELECT sum(p.used_bytes)::int8 AS used, CASE WHEN count(p.pool) > 0 THEN (SELECT setting::int8 * 1024 FROM pg_settings WHERE name = 'sift.shared_cache_kb') END AS lim,
			   sum(p.refusals)::int8 AS refusals
		FROM sift_internal.shared_memory_pools_visible() p
	), q AS (
		SELECT j.*, extract(epoch FROM now() - j.started_at) AS elapsed_s, a.wait_event
		FROM sift_internal.maint_queue() j
		LEFT JOIN pg_index i ON i.indexrelid = j.relation
		LEFT JOIN pg_stat_activity a ON a.pid = j.pid
		WHERE sift._may_inspect(coalesce(i.indrelid, j.relation)::regclass)
	)
	SELECT CASE WHEN q.queued THEN 'queued' ELSE 'worker' END, q.pid, q.relation::regclass, q.part_id, q.job, q.phase, q.priority, q.done, q.total,
		   round(100.0 * q.done / NULLIF(q.total, 0), 2),
		   q.bytes_written, CASE WHEN NOT q.queued THEN (q.elapsed_s * 1000)::int8 END,
		   CASE WHEN NOT q.queued THEN round(q.bytes_written / 1048576.0 / NULLIF(q.elapsed_s, 0), 2) END,
		   CASE WHEN NOT q.queued AND q.done > 0 THEN round((q.elapsed_s * (q.total - q.done) / q.done)::numeric, 1) END,
		   q.started_at, q.wait_event,
		   pc.workers_active, pc.workers_max, pc.pacer_mode, pc.pacer_delay_ms, pc.oltp_active_backends,
		   shm.used, shm.lim, round(100.0 * shm.used / NULLIF(shm.lim, 0), 2), shm.refusals
	FROM q CROSS JOIN pc CROSS JOIN shm
	UNION ALL
	SELECT 'summary', NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		   pc.workers_active, pc.workers_max, pc.pacer_mode, pc.pacer_delay_ms, pc.oltp_active_backends,
		   shm.used, shm.lim, round(100.0 * shm.used / NULLIF(shm.lim, 0), 2), shm.refusals
	FROM pc CROSS JOIN shm
$fn$;

-- The internal helpers that no function or view of a role calls are not executable by every role: the aggregates of sift.argmax and sift.argmin, the triggers of the catalogs and the event triggers.
-- The others are called by the SECURITY INVOKER functions and views of the extension and stay open: each of them answers for what the caller may look at.
DO $$
DECLARE
	f regprocedure;
BEGIN
	FOR f IN SELECT p.oid::regprocedure FROM pg_proc p WHERE p.pronamespace = 'sift_internal'::regnamespace
			 AND p.proname ~ '^(argext_|argmax_|argmin_|projection_defs_|table_paths_skip|ttl_policies_skip|tier_policies_forget)' LOOP
		EXECUTE format('REVOKE EXECUTE ON FUNCTION %s FROM PUBLIC', f);
	END LOOP;
END $$;

-- The filter of sift.projection_filter_served runs with the rights of the owner of the table: only the definer functions that pass the filter the owner stored in sift.projections call it
REVOKE EXECUTE ON FUNCTION sift.projection_filter_served(regclass, text, regclass) FROM PUBLIC;

-- A definition locked for a change is the owner's: another role neither reads a projection of a table it may not look at nor holds the lock of one it does not own
CREATE OR REPLACE FUNCTION sift._projection_get(pname text) RETURNS sift.projections
	LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
DECLARE
	p sift.projections;
BEGIN
	SELECT * INTO p FROM sift.projections x WHERE x.name = pname AND sift._may_inspect_outer(to_regclass(x.source));
	IF FOUND THEN
		PERFORM sift._require_owner(to_regclass(p.source));
		SELECT * INTO p FROM sift.projections x WHERE x.name = pname FOR UPDATE;
	END IF;
	RETURN p;
END $$;
COMMENT ON FUNCTION sift._projection_get(text) IS 'Internal: a definition of sift.projections locked for a change, for the owner of its table; a row with no name for a table the caller may not look at';
