\echo Use "ALTER EXTENSION pg_sift UPDATE TO '3.3'" to load this file. \quit

-- Rewrites the segments of a sift_col index whose pre-aggregates are not what its options ask now (preagg, rollup, cube_minmax take effect for the
-- units written afterwards only), one segment at a time, each an atomic replacement under the maintenance lock that readers and writers do not wait
-- for. max_segments bounds the segments rewritten by this call (NULL: all, 0: only count); the call is made again to go on after a cancel or a crash.
-- refreshed: segments rewritten; remaining: stale segments this call left. The rewrite writes what sift.col_format_minor allows (4 or more).
CREATE FUNCTION sift.col_refresh(index regclass, max_segments int4 DEFAULT NULL, OUT refreshed int4, OUT remaining int4)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_col_refresh' LANGUAGE C VOLATILE;

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

-- What the Custom Scan (SiftAgg) scans of this session did (docs/USAGE.md "Session statistics"): one row per counter, and one per refusal
-- slug recorded (sift.explain_refusals). The counters are in the memory of the backend: no shared memory, no shared_preload_libraries, so the
-- figures are of the session and lost with it.
CREATE FUNCTION sift.stats(OUT name text, OUT value int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_stats' LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sift.stats_reset() RETURNS void
	AS 'MODULE_PATHNAME', 'sift_stats_reset' LANGUAGE C STRICT VOLATILE;

-- Why each candidate path (arm) of the aggregate pushdown kept or left the normal plan for one SELECT, planned and not run (docs/USAGE.md "Why a
-- query kept the normal plan"): one row per arm, used = the arm is in the plan, slug and reason = the first reason it gave up for (NULL when used),
-- scan_cost and normal_cost = the costs of the path that was built and of the normal plan it was compared with (NULL when there was no such path).
-- Needs the privilege to select from the tables of the query; it changes no setting of the session.
CREATE FUNCTION sift.explain_why(query text, OUT arm text, OUT used bool, OUT slug text, OUT reason text, OUT scan_cost float8, OUT normal_cost float8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_agg_explain_why' LANGUAGE C STRICT VOLATILE;

-- The scans of this session that could not answer from the pre-aggregates of their index, by index and reason (a slug), the units scanned for
-- lack of a rollup, and whether other options of the index (rollup, preagg) would answer them (fixable). Most scans first.
CREATE FUNCTION sift.col_rollup_misses(OUT rel regclass, OUT reason text, OUT fixable bool, OUT scans int8, OUT units int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_rollup_misses' LANGUAGE C STRICT VOLATILE;

-- Operator classes that let the planner use a sift_col index as a bitmap index scan (docs/COL-SCAN.md; sift.enable_col_scan, off by
-- default). Opt-in per column: CREATE INDEX ON t USING sift_col (id sift.col_int4_ops, ts sift.col_timestamptz_ops). The stored index
-- is the one of sift.col_ops. Strategies are btree's: 1 <, 2 <=, 3 =, 4 >=, 5 >; text and uuid keep the equality only (their order is
-- the collation's, not the dictionary's).
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
	FOREACH t IN ARRAY ARRAY['date', 'timestamp', 'timestamptz'] LOOP
		list := '';
		FOR n IN 1..5 LOOP
			list := list || format('%sOPERATOR %s pg_catalog.%s (%s, %s)', CASE WHEN n > 1 THEN ', ' ELSE '' END, n, ops[n], t, t);
		END LOOP;
		EXECUTE format('CREATE OPERATOR CLASS sift.col_%s_ops FOR TYPE %s USING sift_col AS %s', t, t, list);
	END LOOP;
	FOREACH t IN ARRAY ARRAY['text', 'uuid'] LOOP
		EXECUTE format('CREATE OPERATOR CLASS sift.col_%s_ops FOR TYPE %s USING sift_col AS OPERATOR 3 pg_catalog.= (%s, %s)', t, t, t, t);
	END LOOP;
END
$$;
