\echo Use "ALTER EXTENSION pg_sift UPDATE TO '3.3'" to load this file. \quit

-- Rewrites the segments of a sift_col index whose pre-aggregates are not what its options ask now (preagg, rollup, cube_minmax take effect for the
-- units written afterwards only), one segment at a time, each an atomic replacement under the maintenance lock that readers and writers do not wait
-- for. max_segments bounds the segments rewritten by this call (NULL: all, 0: only count); the call is made again to go on after a cancel or a crash.
-- refreshed: segments rewritten; remaining: stale segments this call left. The rewrite writes what sift.col_format_minor allows (4 or more).
CREATE FUNCTION sift.col_refresh(index regclass, max_segments int4 DEFAULT NULL, OUT refreshed int4, OUT remaining int4)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_col_refresh' LANGUAGE C VOLATILE;
COMMENT ON FUNCTION sift.col_refresh(regclass, int4) IS 'Rewrites the segments of a sift_col index whose pre-aggregates differ from its current options, at most max_segments per call; returns the segments rewritten and those left.';

-- How well the rows of a sift_col index are ordered for its zone maps (docs/USAGE.md "Is the index clustered"), from the zones of its immutable
-- segments, one row per leaf. units: the units of the index; zoned_units: those whose zone holds a bounded range of the leaf (a unit with an
-- overflow value is always read, one with no value never); useful_pct: the share of them whose range is narrower than the leaf's; point_units:
-- the units an equality on a value of the data touches, on average; range_units: the same for a range of width_pct percent of the leaf's span;
-- range_ideal: what a column sorted by itself would touch; correlation: the order of the units against the order of their values (1 ascending,
-- -1 descending, 0 scattered); verdict: clustered, partial or scattered. Integer, date and timestamp leaves only (the others have NULLs).
CREATE FUNCTION sift.col_clustering(index regclass, width_pct float8 DEFAULT 1,
									OUT leaf int4, OUT path text, OUT units int8, OUT zoned_units int8, OUT useful_pct float8,
									OUT point_units float8, OUT range_units float8, OUT range_ideal float8, OUT correlation float8,
									OUT verdict text)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_clustering' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.col_clustering(regclass, float8) IS 'How well the rows of a sift_col index are ordered for its zone maps, one row per leaf: the units an equality or a range touches, the correlation and a verdict (clustered, partial or scattered).';

-- The sift_col indexes that a REINDEX would improve, one row per reason: an option (cube_minmax, JSON paths, rollup_dims, set_columns) that only the
-- units written after it follow, set on an index whose older units lack the section (docs/UPGRADING.md). detected: the option and the oldest column
-- format; advice: what the REINDEX changes; command: the statement to run (REINDEX CONCURRENTLY cannot run in a transaction block). Reads the catalogs
-- and the metapages, changes nothing, and gives up on an index that is locked at once (lock_timeout) or that this library refuses (sift.upgrade_check
-- reports those); a library that cannot be loaded is an error. ALTER EXTENSION pg_sift UPDATE to 3.3 prints this list.
CREATE FUNCTION sift.reindex_needed(OUT index regclass, OUT detected text, OUT advice text, OUT command text)
	RETURNS SETOF record
	LANGUAGE plpgsql SET lock_timeout = '100ms' AS $fn$
DECLARE
	r record;
	o record;
	lo int;
BEGIN
	FOR r IN SELECT c.oid AS relid, c.reloptions AS opts, format('%I.%I', n.nspname, c.relname) AS name
			 FROM pg_class c JOIN pg_am am ON am.oid = c.relam JOIN pg_namespace n ON n.oid = c.relnamespace
			 WHERE am.amname = 'sift_col' AND c.relkind = 'i' AND c.reloptions IS NOT NULL ORDER BY n.nspname, c.relname
	LOOP
		BEGIN
			SELECT min((to_jsonb(s)->>'format')::int) INTO lo FROM sift.col_segments(r.relid) s;
		EXCEPTION WHEN OTHERS THEN
			IF SQLSTATE IN ('42883', '58P01', '55P03') THEN RAISE; END IF;
			CONTINUE;
		END;
		CONTINUE WHEN lo IS NULL;
		-- a segment is minor 9 or 10 when some unit holds the section, so a lower minor means no unit of the segment does (docs/COL.md 2.13)
		FOR o IN SELECT * FROM (VALUES
					('^paths=', 6, 'JSON paths',
					 'segments before format 6 may lack the kinds of a JSON null or of a type; a predicate that needs them uses the normal plan, and a REINDEX writes them'),
					('^cube_minmax=(true|on)$', 7, 'cube_minmax',
					 'the units written before the option scan; a REINDEX writes the minimum and maximum of every unit'),
					('^rollup_dims=.', 9, 'rollup_dims',
					 'the units written before the option have no rollup of their rows and are scanned (nor has a unit over the caps of the rollup, which a REINDEX does not change); a REINDEX writes it where it fits'),
					('^set_columns=.', 10, 'set_columns',
					 'the units written before the option have no small-set summary and are read (nor has a unit whose keys are all alike or fill their range); a REINDEX writes it where it helps')
				 ) v(pat, minor, what, why)
				 WHERE lo < v.minor AND EXISTS (SELECT 1 FROM unnest(r.opts) opt WHERE opt ~ v.pat) ORDER BY v.minor
		LOOP
			index := r.relid; detected := format('%s with column format %s', o.what, lo); advice := o.why;
			command := format('REINDEX INDEX CONCURRENTLY %s;', r.name);
			RETURN NEXT;
		END LOOP;
	END LOOP;
END
$fn$;
COMMENT ON FUNCTION sift.reindex_needed() IS 'The sift_col indexes that a REINDEX would improve (an option that only the units written after it follow), with the statement to run; changes nothing.';

-- What the Custom Scan (SiftAgg) scans of this session did (docs/USAGE.md "Session statistics"): one row per counter, and one per refusal
-- slug recorded (sift.explain_refusals). The counters are in the memory of the backend: no shared memory, no shared_preload_libraries, so the
-- figures are of the session and lost with it.
CREATE FUNCTION sift.stats(OUT name text, OUT value int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_stats' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.stats() IS 'What the aggregate pushdown (Custom Scan (SiftAgg)) scans of this session did: one row per counter and per refusal slug.';

CREATE FUNCTION sift.stats_reset() RETURNS void
	AS 'MODULE_PATHNAME', 'sift_stats_reset' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.stats_reset() IS 'Resets the session counters that sift.stats() reports.';

-- Why each candidate path (arm) of the aggregate pushdown kept or left the normal plan for one SELECT, planned and not run (docs/USAGE.md "Why a
-- query kept the normal plan"): one row per arm, used = the arm is in the plan, slug and reason = the first reason it gave up for (NULL when used),
-- scan_cost and normal_cost = the costs of the path that was built and of the normal plan it was compared with (NULL when there was no such path).
-- Needs the privilege to select from the tables of the query; it changes no setting of the session.
CREATE FUNCTION sift.explain_why(query text, OUT arm text, OUT used bool, OUT slug text, OUT reason text, OUT scan_cost float8, OUT normal_cost float8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_agg_explain_why' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.explain_why(text) IS 'Why each candidate path of the aggregate pushdown kept or left the normal plan for one SELECT, planned and not run: one row per arm with the first reason it gave up.';

-- The slugs sift.explain_why and sift.stats() name, with their class: 'limit' when the query has more of something than a fixed limit of the
-- extension allows (docs/LIMITS.md), 'semantic' otherwise.
CREATE FUNCTION sift.refusal_slugs(OUT slug text, OUT class text, OUT reason text)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_agg_refusal_slugs' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.refusal_slugs() IS 'The slugs that sift.explain_why and sift.stats() name, with their class (limit: the query has more of something than a fixed limit of the extension allows; semantic: otherwise) and the reason.';

-- The scans of this session that could not answer from the pre-aggregates of their index, by index and reason (a slug), the units scanned for
-- lack of a rollup, and whether other options of the index (rollup, preagg) would answer them (fixable). Most scans first.
CREATE FUNCTION sift.col_rollup_misses(OUT rel regclass, OUT reason text, OUT fixable bool, OUT scans int8, OUT units int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_rollup_misses' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.col_rollup_misses() IS 'The scans of this session that could not answer from the pre-aggregates of their sift_col index, by index and reason, and whether other index options would answer them.';

-- Operator classes that let the planner use a sift_col index as a bitmap index scan (docs/COL-SCAN.md; sift.enable_col_scan, off by
-- default). Opt-in per column: CREATE INDEX ON t USING sift_col (id sift.col_int4_ops, ts sift.col_timestamptz_ops). The stored index
-- is the one of sift.col_ops. Strategies are btree's: 1 <, 2 <=, 3 =, 4 >=, 5 >; text and uuid keep the equality only (their order is
-- the collation's, not the dictionary's), text adds 7 LIKE and 8 ^@ (a literal prefix); bpchar is char(n) only, numeric numeric(p <= 18, s) only (CREATE INDEX refuses the others).
DO $$
DECLARE
	t text;
	u text;
	n int;
	ops text[] := ARRAY['<', '<=', '=', '>=', '>'];
	ints text[] := ARRAY['int2', 'int4', 'int8'];
	list text;
BEGIN
	CREATE OPERATOR FAMILY sift.col_integer_ops USING sift_col;
	FOREACH t IN ARRAY ints LOOP
		list := '';
		FOR n IN 1..5 LOOP
			list := list || format('%sOPERATOR %s pg_catalog.%s (%s, %s)', CASE WHEN n > 1 THEN ', ' ELSE '' END, n, ops[n], t, t);
		END LOOP;
		EXECUTE format('CREATE OPERATOR CLASS sift.col_%s_ops FOR TYPE %s USING sift_col FAMILY sift.col_integer_ops AS %s', t, t, list);
	END LOOP;
	FOREACH t IN ARRAY ints LOOP
		FOREACH u IN ARRAY ints LOOP
			CONTINUE WHEN t = u;
			list := '';
			FOR n IN 1..5 LOOP
				list := list || format('%sOPERATOR %s pg_catalog.%s (%s, %s)', CASE WHEN n > 1 THEN ', ' ELSE '' END, n, ops[n], t, u);
			END LOOP;
			EXECUTE format('ALTER OPERATOR FAMILY sift.col_integer_ops USING sift_col ADD %s', list);
		END LOOP;
	END LOOP;
	CREATE OPERATOR FAMILY sift.col_float_ops USING sift_col;
	FOREACH t IN ARRAY ARRAY['float4', 'float8'] LOOP
		list := '';
		FOR n IN 1..5 LOOP
			list := list || format('%sOPERATOR %s pg_catalog.%s (%s, %s)', CASE WHEN n > 1 THEN ', ' ELSE '' END, n, ops[n], t, t);
		END LOOP;
		EXECUTE format('CREATE OPERATOR CLASS sift.col_%s_ops FOR TYPE %s USING sift_col FAMILY sift.col_float_ops AS %s', t, t, list);
	END LOOP;
	FOREACH t IN ARRAY ARRAY['float4', 'float8'] LOOP
		FOREACH u IN ARRAY ARRAY['float4', 'float8'] LOOP
			CONTINUE WHEN t = u;
			list := '';
			FOR n IN 1..5 LOOP
				list := list || format('%sOPERATOR %s pg_catalog.%s (%s, %s)', CASE WHEN n > 1 THEN ', ' ELSE '' END, n, ops[n], t, u);
			END LOOP;
			EXECUTE format('ALTER OPERATOR FAMILY sift.col_float_ops USING sift_col ADD %s', list);
		END LOOP;
	END LOOP;
	FOREACH t IN ARRAY ARRAY['date', 'timestamp', 'timestamptz', 'bool', 'numeric'] LOOP
		list := '';
		FOR n IN 1..5 LOOP
			list := list || format('%sOPERATOR %s pg_catalog.%s (%s, %s)', CASE WHEN n > 1 THEN ', ' ELSE '' END, n, ops[n], t, t);
		END LOOP;
		EXECUTE format('CREATE OPERATOR CLASS sift.col_%s_ops FOR TYPE %s USING sift_col AS %s', t, t, list);
	END LOOP;
	EXECUTE 'CREATE OPERATOR CLASS sift.col_text_ops FOR TYPE text USING sift_col AS OPERATOR 3 pg_catalog.= (text, text), OPERATOR 7 pg_catalog.~~ (text, text), OPERATOR 8 pg_catalog.^@ (text, text)';
	FOREACH t IN ARRAY ARRAY['bpchar', 'uuid'] LOOP
		EXECUTE format('CREATE OPERATOR CLASS sift.col_%s_ops FOR TYPE %s USING sift_col AS OPERATOR 3 pg_catalog.= (%s, %s)', t, t, t, t);
	END LOOP;
END
$$;

-- sift.table_space reports the virtual blocks of TIDs left: the function is replaced, its columns changed.
DROP FUNCTION sift.table_space(regclass);

-- The space of a sift_table: relation_blocks (the file), high_water (blocks ever used), free_blocks (on the free list, of which
-- reusable_blocks can be taken now, the rest wait for snapshots that may still read them), free_extents, vblocks_left (the virtual
-- blocks of TIDs not handed out yet, up to 291 rows each; they are never reused, and VACUUM FULL renumbers them).
CREATE FUNCTION sift.table_space(tbl regclass,
								 OUT relation_blocks int8, OUT high_water int8, OUT free_blocks int8, OUT reusable_blocks int8,
								 OUT free_extents int8, OUT vblocks_left int8)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_table_space' LANGUAGE C STRICT VOLATILE;

-- A column added to a sift_table past the 127 it holds (or a column of declared paths renamed or retyped into the excess) fails the ALTER
-- TABLE, as CREATE TABLE does, instead of the next write.
CREATE FUNCTION sift.table_ddl_check() RETURNS event_trigger
	AS 'MODULE_PATHNAME', 'sift_table_ddl_check' LANGUAGE C;

CREATE EVENT TRIGGER sift_table_ddl_check ON ddl_command_end WHEN TAG IN ('ALTER TABLE') EXECUTE FUNCTION sift.table_ddl_check();

-- The indexes this update leaves for a REINDEX, printed once; sift.reindex_needed() lists them again. The last step of every update path to 3.3, so it
-- covers the indexes that 3.0, 3.1 and 3.2 wrote. It only reads, and never fails the update: a library that does not answer (an older one loaded in this
-- session, an index locked by another session) gets a warning that says to run the function after reconnecting. An extension script runs with
-- client_min_messages at warning or above, so what asks for an action is a warning, and the all-clear lowers it to notice for itself (undone with the script).
DO $do$
DECLARE
	cmds text[];
	n int;
BEGIN
	SELECT array_agg(command ORDER BY command), count(*) INTO cmds, n FROM (SELECT DISTINCT command FROM sift.reindex_needed()) s;
	IF n = 0 THEN
		PERFORM set_config('client_min_messages', 'notice', true);
		RAISE NOTICE 'pg_sift 3.3: no index needs a REINDEX after this update';
	ELSE
		RAISE WARNING 'pg_sift 3.3: REINDEX is worth it for % sift_col index(es) after this update', n
			USING DETAIL = concat(array_to_string(cmds[1:20], E'\n'), CASE WHEN n > 20 THEN format(E'\nand %s more', n - 20) END),
				  HINT = 'The older units stay readable until then. REINDEX CONCURRENTLY cannot run in a transaction block. SELECT * FROM sift.reindex_needed() gives the reason for each index.';
	END IF;
EXCEPTION WHEN OTHERS THEN
	RAISE WARNING 'pg_sift 3.3: the check for indexes to REINDEX did not run (%): reconnect and run SELECT * FROM sift.reindex_needed()', SQLERRM;
END
$do$;

-- Maintenance state of an index: segments and sealed write buffers waiting for promotion, running (a promotion, merge, VACUUM or verify holds
-- the maintenance lock; NULL on a standby) and, when its last background step failed, the number of failures in a row, when, the earliest the
-- background tries again, the SQLSTATE and the message (the log has them too, this keeps the last). NULL while a REINDEX holds the index.
CREATE FUNCTION sift.worker_state(index regclass,
								  OUT segments int4, OUT sealed int4, OUT running bool, OUT failures int4,
								  OUT failed_at timestamptz, OUT next_retry timestamptz, OUT error_code text, OUT last_error text)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_worker_state' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.worker_state(regclass) IS 'Maintenance state of an index: segments and sealed write buffers waiting for promotion, whether a step runs now, and the last background failure with its retry time.';

-- One row per sift and sift_col index of the database whose table the caller can read; due: why a sift_col index would be rewritten now
-- (sift.col_maintenance_due). A sift_col step that fails raises its error to the worker, which the log records: failures stay 0 for it.
CREATE VIEW sift.workers AS
	SELECT c.oid::regclass AS index, a.amname::text AS kind, w.segments, w.sealed, w.running, w.failures, w.failed_at, w.next_retry,
		   w.error_code, w.last_error,
		   CASE WHEN a.amname = 'sift_col' THEN sift.col_maintenance_due(c.oid) END AS due
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname IN ('sift', 'sift_col')
	JOIN pg_index i ON i.indexrelid = c.oid
	CROSS JOIN LATERAL sift.worker_state(c.oid) w
	WHERE has_table_privilege(i.indrelid, 'SELECT') AND w.segments IS NOT NULL;
COMMENT ON VIEW sift.workers IS 'One row per sift and sift_col index the caller can read: its maintenance state (sift.worker_state) and, for a sift_col index, why it would be rewritten now.';

-- Logical replication of a sift_table needs pg_sift in shared_preload_libraries (docs/COL-TAM.md section 5): a custom resource manager can only
-- be registered at preload time. Without it the first DML on a published table fails; this trigger refuses CREATE/ALTER PUBLICATION instead, when
-- the publication would carry a sift_table (wal_level = logical, a permanent table, a publication that publishes inserts, updates or deletes).
CREATE FUNCTION sift.publication_check() RETURNS event_trigger
	LANGUAGE plpgsql AS $fn$
DECLARE
	tbl regclass;
BEGIN
	IF current_setting('wal_level') <> 'logical' OR current_setting('shared_preload_libraries') ~ '(^|[\s,"/])pg_sift([\s,".]|$)' THEN
		RETURN;
	END IF;
	SELECT g.relid::regclass INTO tbl
		FROM pg_event_trigger_ddl_commands() d
			JOIN pg_publication p ON p.oid = CASE d.classid
					WHEN 'pg_publication'::regclass THEN d.objid
					WHEN 'pg_publication_rel'::regclass THEN (SELECT r.prpubid FROM pg_publication_rel r WHERE r.oid = d.objid)
					WHEN 'pg_publication_namespace'::regclass THEN (SELECT n.pnpubid FROM pg_publication_namespace n WHERE n.oid = d.objid)
				END
			CROSS JOIN LATERAL pg_get_publication_tables(p.pubname) g
			JOIN pg_class c ON c.oid = g.relid
			JOIN pg_am a ON a.oid = c.relam
		WHERE a.amname = 'sift_table' AND c.relpersistence = 'p' AND (p.pubinsert OR p.pubupdate OR p.pubdelete)
		LIMIT 1;
	IF tbl IS NOT NULL THEN
		RAISE EXCEPTION 'sift_table table "%" is published and wal_level is "logical", but pg_sift is not in shared_preload_libraries', tbl
			USING ERRCODE = 'feature_not_supported',
				DETAIL = 'Logical replication of a sift_table table needs the pg_sift resource manager, which is registered only when the library is preloaded.',
				HINT = format('Add pg_sift to shared_preload_libraries (primary and every standby) and restart, or publish a heap staging table instead of "%s".', tbl);
	END IF;
END
$fn$;

CREATE EVENT TRIGGER sift_publication_check ON ddl_command_end WHEN TAG IN ('CREATE PUBLICATION', 'ALTER PUBLICATION')
	EXECUTE FUNCTION sift.publication_check();
