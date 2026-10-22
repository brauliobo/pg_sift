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

-- The scans of this session that could not answer from the pre-aggregates of their index, by index and reason (a slug), the units scanned for
-- lack of a rollup, and whether other options of the index (rollup, preagg) would answer them (fixable). Most scans first.
CREATE FUNCTION sift.col_rollup_misses(OUT rel regclass, OUT reason text, OUT fixable bool, OUT scans int8, OUT units int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_rollup_misses' LANGUAGE C STRICT VOLATILE;
