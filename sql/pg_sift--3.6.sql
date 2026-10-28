\echo Use "CREATE EXTENSION pg_sift" to load this file. \quit

-- Every role may use the schema: the operator and the access methods need nothing of it, sift.score, the diagnostics and the maintenance functions do, and
-- each of them checks what it touches (owner or MAINTAIN for maintenance, SELECT for the values of a table). REVOKE USAGE ON SCHEMA sift FROM PUBLIC takes it back.
GRANT USAGE ON SCHEMA sift TO PUBLIC;

CREATE FUNCTION sift.sift_handler(internal) RETURNS index_am_handler
	AS 'MODULE_PATHNAME', 'sift_handler' LANGUAGE C STRICT;

CREATE ACCESS METHOD sift TYPE INDEX HANDLER sift.sift_handler;

CREATE FUNCTION sift.match_support(internal) RETURNS internal
	AS 'MODULE_PATHNAME', 'sift_match_support' LANGUAGE C STRICT;

CREATE FUNCTION sift.matchsel(internal, oid, internal, integer) RETURNS float8
	AS 'MODULE_PATHNAME', 'sift_matchsel' LANGUAGE C STRICT STABLE PARALLEL SAFE;

-- Default analyzer; the planner support function rebinds calls on columns
-- whose sift index uses another analyzer to sift.match_idx.
CREATE FUNCTION sift.match(doc text, query text) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_match'
	LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE COST 1000 SUPPORT sift.match_support;

CREATE FUNCTION sift.match_idx(doc text, query text, index oid) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_match_idx'
	LANGUAGE C STRICT STABLE PARALLEL SAFE COST 1000 SUPPORT sift.match_support;

-- In pg_catalog so `col @@> 'query'` works without changing search_path.
CREATE OPERATOR pg_catalog.@@> (
	LEFTARG = text,
	RIGHTARG = text,
	FUNCTION = sift.match,
	RESTRICT = sift.matchsel,
	JOIN = contjoinsel
);

CREATE OPERATOR CLASS sift.text_ops DEFAULT FOR TYPE text USING sift AS
	OPERATOR 1 pg_catalog.@@> (text, text);

-- Computed by the sift scan and projected through the plan; calling them
-- anywhere else raises an error.
CREATE FUNCTION sift.score(tid) RETURNS real
	AS 'MODULE_PATHNAME', 'sift_score' LANGUAGE C STRICT STABLE PARALLEL SAFE;

CREATE FUNCTION sift.max_score(tid) RETURNS real
	AS 'MODULE_PATHNAME', 'sift_max_score' LANGUAGE C STRICT STABLE PARALLEL SAFE;

CREATE FUNCTION sift.highlight(doc text, query text,
							   start_tag text DEFAULT '<b>', stop_tag text DEFAULT '</b>',
							   index regclass DEFAULT NULL) RETURNS text
	AS 'MODULE_PATHNAME', 'sift_highlight' LANGUAGE C STABLE PARALLEL SAFE;

-- Analyzer of index (or the default one), overridden by non-NULL options;
-- offsets are 0-based character offsets [start, end) into doc.
CREATE FUNCTION sift.tokenize(doc text, index regclass DEFAULT NULL,
							  stemmer text DEFAULT NULL, case_folding bool DEFAULT NULL,
							  accent_folding bool DEFAULT NULL,
							  OUT token text, OUT pos int4,
							  OUT start_offset int4, OUT end_offset int4)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_tokenize' LANGUAGE C STABLE PARALLEL SAFE;

CREATE FUNCTION sift.parse_query(query text, index regclass DEFAULT NULL) RETURNS text
	AS 'MODULE_PATHNAME', 'sift_parse_query' LANGUAGE C STABLE PARALLEL SAFE;

CREATE FUNCTION sift.promote(index regclass) RETURNS int4
	AS 'MODULE_PATHNAME', 'sift_promote' LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sift.merge(index regclass, force bool DEFAULT false) RETURNS int4
	AS 'MODULE_PATHNAME', 'sift_merge' LANGUAGE C STRICT VOLATILE;

-- One row per segment; the section columns are NULL for write buffers (extents and dead_docs too).
-- dead_docs: documents of an immutable segment that VACUUM has marked dead (rows of rolled-back
-- transactions and deleted rows still count in ndocs, the document frequencies and avgdl until then).
CREATE FUNCTION sift.segments(index regclass,
							  OUT gen int8, OUT kind text, OUT ndocs int8, OUT sum_dl int8,
							  OUT avgdl float8, OUT bytes int8, OUT sb_lo int4, OUT sb_hi int4,
							  OUT dirty bool, OUT nterms int8, OUT avgdl_ref float8,
							  OUT docmap_bytes int8, OUT postings_bytes int8, OUT dict_bytes int8,
							  OUT live_bytes int8, OUT extents int4, OUT dead_docs int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_segments' LANGUAGE C STRICT VOLATILE;

-- Postings by segment and df class (decodes every term): representation
-- counts, codec and sidecar (tf, bounds) bytes, bits per posting.
CREATE FUNCTION sift.segment_postings(index regclass,
									  OUT gen int8, OUT df_class text, OUT terms int8,
									  OUT postings int8, OUT inline_terms int8, OUT list_terms int8,
									  OUT grouped_terms int8, OUT codec_bytes int8,
									  OUT sidecar_bytes int8, OUT bits_per_posting float8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_segment_postings' LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sift.verify(index regclass, heapallindexed bool DEFAULT false) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_verify' LANGUAGE C STRICT VOLATILE;

-- Keyword facets (DESIGN.md item 11): the number of matches of a query per value of a keyword index.
-- A keyword index is CREATE INDEX ... USING sift (col) WITH (analyzer = keyword).

-- Counts per value of facet_index among the visible rows that index matches query for, count
-- descending then value; lim NULL = every value, nulls = also a row with a NULL value counting the
-- matches without one (it makes every value be counted, so lim cannot end the search early).
CREATE FUNCTION sift.facets(index regclass, facet_index regclass, query text,
							lim int DEFAULT 10, nulls bool DEFAULT false,
							OUT value text, OUT count int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_facet_counts' LANGUAGE C STABLE;

-- The space and directory state of an index. relation_blocks: size of the relation; high_water: blocks
-- ever allocated; free_blocks: blocks on the free list, of which reusable_blocks can be taken now (the
-- rest wait for the snapshots that may still read them); maintenance_busy: a promotion, merge, VACUUM or
-- verify holds the maintenance lock (NULL on a standby).
CREATE FUNCTION sift.index_info(index regclass,
								OUT relation_blocks int8, OUT high_water int8, OUT free_blocks int8,
								OUT reusable_blocks int8, OUT free_extents int8, OUT segments int4,
								OUT max_segments int4, OUT ndocs int8, OUT sum_dl int8,
								OUT maintenance_busy bool)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_index_info' LANGUAGE C STRICT VOLATILE;

-- The version of the loaded library; the installed extension's is in pg_extension.
CREATE FUNCTION sift.version() RETURNS text
	AS 'MODULE_PATHNAME', 'sift_version' LANGUAGE C STABLE;

-- The sift_col index access method (docs/COL.md): a columnar index over an existing heap table.

CREATE FUNCTION sift.col_handler(internal) RETURNS index_am_handler
	AS 'MODULE_PATHNAME', 'sift_col_handler' LANGUAGE C STRICT;

CREATE ACCESS METHOD sift_col TYPE INDEX HANDLER sift.col_handler;

-- No operators: the index is read by the column scans, not through search operators. An operator class needs one member, which
-- the access method never calls.
CREATE FUNCTION sift.col_ops_unused(anyelement) RETURNS void
	LANGUAGE sql IMMUTABLE PARALLEL SAFE AS 'SELECT NULL::void';

CREATE OPERATOR CLASS sift.col_ops DEFAULT FOR TYPE anyelement USING sift_col AS FUNCTION 1 sift.col_ops_unused(anyelement);

-- Rows are the live rows of the segments; units are (segment, 256-page superblock) pairs. simd: the kernel level the scans use.
CREATE FUNCTION sift.col_index_info(index regclass,
									OUT relation_blocks int8, OUT high_water int8, OUT free_blocks int8,
									OUT reusable_blocks int8, OUT free_extents int8, OUT segments int4,
									OUT max_segments int4, OUT rows int8, OUT bytes int8, OUT units int8,
									OUT buffers int4, OUT leaves int4, OUT maintenance_busy bool,
									OUT simd text, OUT preagg_bytes int8, OUT preagg_units int8)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_col_index_info' LANGUAGE C STRICT VOLATILE;

-- The pre-aggregates of every stored unit (docs/COL-PREAGG.md): the aggregate of a leaf, and for a dimension the histogram of its groups with the
-- count and sum of one measure. exact: the unit may answer from them (clean, every page all-visible). Debug and test hooks.
CREATE FUNCTION sift.col_unit_agg(index regclass, leaf text,
								  OUT segment int8, OUT sb int4, OUT rows int4, OUT exact bool, OUT nvals int8, OUT nnull int8,
								  OUT min text, OUT max text, OUT sum numeric, OUT sumsq numeric, OUT sum_float float8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_unit_aggs_sql' LANGUAGE C VOLATILE;

CREATE FUNCTION sift.col_unit_hist(index regclass, dim text, measure text DEFAULT NULL,
								   OUT segment int8, OUT sb int4, OUT exact bool, OUT key text, OUT n int8,
								   OUT nn int8, OUT sum numeric, OUT sum_float float8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_unit_hists_sql' LANGUAGE C VOLATILE;

-- One row per segment; the immutable-only columns are NULL for write buffers. format: the column format minor of the segment (3 as
-- pg_sift 3.0 writes it, 4 with the type tag and the element cubes; docs/COL.md 2), preagg_bytes: the bytes of its pre-aggregates.
CREATE FUNCTION sift.col_segments(index regclass,
								  OUT gen int8, OUT kind text, OUT rows int8, OUT bytes int8,
								  OUT sb_lo int4, OUT sb_hi int4, OUT units int4, OUT dirty bool,
								  OUT dead_rows int8, OUT extents int4, OUT format int4, OUT preagg_bytes int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_segments' LANGUAGE C STRICT VOLATILE;

-- One row per leaf: its chunks by encoding (const, for, pack, other, text stored inline), their bytes, and the bytes of its dictionaries (text leaves).
CREATE FUNCTION sift.col_leaves(index regclass,
								OUT leaf int4, OUT path text, OUT kind text, OUT type text,
								OUT const_chunks int8, OUT for_chunks int8, OUT pack_chunks int8, OUT codec_chunks int8,
								OUT inline_chunks int8, OUT chunk_bytes int8, OUT dict_bytes int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_leaves' LANGUAGE C STRICT VOLATILE;

-- Where the bytes of a sift_col index or a sift_table go, one row per kind: chunks and dict per leaf; unit_dir, dict_other, zones, preagg,
-- sbdir_schema, page_overhead, headers (segment structure), free (blocks on the free list) and other (metapage, directory, write buffer, tombstones).
-- The bytes add up to pg_relation_size.
CREATE FUNCTION sift.col_sections(rel regclass, OUT section text, OUT leaf text, OUT bytes int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_sections' LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sift.col_verify(index regclass, heapallindexed bool DEFAULT false) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_col_verify' LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sift.col_promote(index regclass) RETURNS int4
	AS 'MODULE_PATHNAME', 'sift_col_promote' LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sift.col_merge(index regclass, force bool DEFAULT false) RETURNS int4
	AS 'MODULE_PATHNAME', 'sift_col_merge' LANGUAGE C STRICT VOLATILE;

-- The rewrite of sift.col_auto_merge, now and in this session (for a job: pg_cron): when the churn has scattered the index (many units with a dead row,
-- many units over the same superblocks, many rows in the write buffer), the write buffer is sealed and every segment merged into one, without
-- blocking readers or writers. False when nothing is due or the maintenance lock is held.
CREATE FUNCTION sift.col_maintain(index regclass) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_col_maintain' LANGUAGE C STRICT VOLATILE;

-- Why sift.col_maintain would rewrite the index now: 'dirty units', 'fragmented units' or 'write buffer rows' (the thresholds are sift.col_auto_merge_*),
-- NULL when it is not due. automatic: as the server's own rewrite, which waits for sift.col_auto_merge_min_churn_pct and for the horizon.
CREATE FUNCTION sift.col_maintenance_due(index regclass, automatic bool DEFAULT true) RETURNS text
	AS 'MODULE_PATHNAME', 'sift_col_maintenance_due' LANGUAGE C STRICT VOLATILE;

-- Where the chain stands that keeps a sift_col index answerable under churn (sift.col_auto_merge): the table is vacuumed when the rows changed since its last
-- VACUUM (changed_rows; NULL while the statistics have no entry) reach vacuum_at, and the index is rewritten after that VACUUM (rewrite_due, as the server's
-- own rule sees it). blocked_by is what keeps a worker from doing either, empty when nothing does.
CREATE FUNCTION sift.col_maintenance_status(index regclass,
											OUT heap regclass, OUT last_vacuum timestamptz, OUT changed_rows int8, OUT vacuum_at int8,
											OUT rewrite_due text, OUT last_request timestamptz, OUT blocked_by text[])
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_col_maintenance_status' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.col_maintenance_status(regclass) IS 'Where the VACUUM and the rewrite that keep a sift_col index answerable stand: the table, its last VACUUM, the rows changed since and the number that starts the next, why the rewrite is due, the last request of a worker, and blocked_by: what keeps a worker from running (setting off, autovacuum_enabled false, a standby, a snapshot holding back the last rewrite).';

-- The profile of the columns of a sift_col index and what its options could still use, one row per column and per pair of dimensions: kind (dimension, key,
-- unique, date, measure, text, list), what the index holds of it, the units looked at (32 evenly spaced ones: the same answer for the same index), the share
-- of them that satisfy the rule of a dimension (at most 256 distinct values) or of a pair (its combinations fit a rollup of the rows of the unit), and the advice
-- when a rule holds in 90% of them. Nothing is changed.
CREATE FUNCTION sift.index_advice(index regclass,
								  OUT leaf text, OUT kind text, OUT held text, OUT sampled int4, OUT share int4, OUT advice text)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_index_advice' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.index_advice(regclass) IS 'The profile of the columns of a sift_col index and what its options could still use: per column its kind (dimension, key, unique, date, measure, text, list), what the index holds of it and, over 32 evenly spaced units, the share that satisfy the rule of a dimension (at most 256 distinct values) or, per pair of dimensions, of a rollup of the rows; advice only, nothing is changed.';

-- The paths option (docs/COL-SHRED.md) that a jsonb column asks for, read from a sample of its documents (16 stretches of blocks evenly spaced over the table, so
-- the same table gives the same answer): a key that is a scalar of one type in most of the documents that have it is a typed path, a map of more than 32 keys of
-- one type a wildcard, an array of objects a group; a rare key or one without a single shape is left out. NULL when no key qualifies. Nothing is changed.
CREATE FUNCTION sift.suggest_paths(rel regclass, column_name text, sample int4 DEFAULT 2000) RETURNS text
	AS 'MODULE_PATHNAME', 'sift_col_suggest_paths' LANGUAGE C VOLATILE;
COMMENT ON FUNCTION sift.suggest_paths(regclass, text, int4) IS 'The paths option that a jsonb column asks for, read from a sample of its documents (evenly spaced blocks, the same answer for the same table): typed paths for the scalar keys, a wildcard for a map of more than 4 keys of one type, a group for an array of objects, free text (values of more than 64 bytes on average) left out; advice only, NULL when no key qualifies.';

-- The sift_table table access method (docs/COL-TAM.md): CREATE TABLE ... USING sift_table.
CREATE FUNCTION sift.table_handler(internal) RETURNS table_am_handler
	AS 'MODULE_PATHNAME', 'sift_table_handler' LANGUAGE C STRICT;

CREATE ACCESS METHOD sift_table TYPE TABLE HANDLER sift.table_handler;

-- One row per segment of a sift_table: kind buffer (the row format write buffer; bytes = its pages), sealed buffer (no more rows,
-- waiting to become columns), columns (a column segment: rows including dead ones, units, dead rows, format = its column format
-- minor, as sift.col_segments; unfrozen: some of its rows are not final and carry their transaction and command, with the smallest and the
-- largest transaction id among them) or tombstones.
CREATE FUNCTION sift.table_segments(tbl regclass,
									OUT gen int8, OUT kind text, OUT rows int8, OUT bytes int8,
									OUT sb_lo int4, OUT sb_hi int4, OUT units int4, OUT dead_rows int8, OUT format int4,
									OUT unfrozen bool, OUT min_xid xid, OUT max_xid xid)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_table_segments' LANGUAGE C STRICT VOLATILE;

-- The space of a sift_table: relation_blocks (the file), high_water (blocks ever used), free_blocks (on the free list, of which
-- reusable_blocks can be taken now, the rest wait for snapshots that may still read them), free_extents, vblocks_left (the virtual
-- blocks of TIDs not handed out yet, up to 291 rows each; they are never reused, and VACUUM FULL renumbers them).
CREATE FUNCTION sift.table_space(tbl regclass,
								 OUT relation_blocks int8, OUT high_water int8, OUT free_blocks int8, OUT reusable_blocks int8,
								 OUT free_extents int8, OUT vblocks_left int8)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_table_space' LANGUAGE C STRICT VOLATILE;

-- VACUUM's work on the column segments and the file of a sift_table, without its indexes (so without the tombstone rewrite, which needs them): the
-- dead rows are cleared, write buffers promoted, mostly dead segments rewritten, free space gathered and the free end of the file returned.
-- busy: the table, or the lock of its end, is in use and the call did nothing there. Background maintenance does the same (sift.table_auto_defrag).
CREATE FUNCTION sift.table_maintain(tbl regclass,
									OUT busy bool, OUT dead_rows int8, OUT promoted int4, OUT merged int4,
									OUT blocks_before int8, OUT blocks_after int8)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_table_maintain' LANGUAGE C STRICT VOLATILE;

-- sift_col: on-demand refresh of the rollups of the units (docs/COL-PREAGG.md section 7). A scan that reads exact units for lack of their rollup
-- (missing, built with other dims, a measure it lacks) queues the segment of each; a background worker rewrites the queued segments with the
-- rollups the index writes now. Counters are of the server since it started: requested, refreshed and skipped are in units, dropped is the
-- units of requests that found the queue full; queued and refreshing are the requests of this database now.
CREATE FUNCTION sift.col_rollup_counters(OUT requested int8, OUT refreshed int8, OUT skipped int8, OUT dropped int8,
										 OUT queued int4, OUT refreshing int4)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_col_rollup_counters' LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sift.col_rollup_queue(OUT index regclass, OUT segment int8, OUT units int4, OUT requested_at timestamptz, OUT state text)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_rollup_queue' LANGUAGE C STRICT VOLATILE;

-- Refreshes the queued requests of the index now, in this session (like sift.col_merge): the segments rewritten.
CREATE FUNCTION sift.col_rollup_refresh(index regclass) RETURNS int4
	AS 'MODULE_PATHNAME', 'sift_col_rollup_refresh' LANGUAGE C STRICT VOLATILE;

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
-- -1 descending, 0 scattered); verdict: clustered, partial or scattered; depth_max: the most units whose zones hold one value (the deepest overlap,
-- what a column that arrives out of order adds up to; point_units is the average, a few deep values hide in it). Integer, date and timestamp leaves only (the others have NULLs).
CREATE FUNCTION sift.col_clustering(index regclass, width_pct float8 DEFAULT 1,
									OUT leaf int4, OUT path text, OUT units int8, OUT zoned_units int8, OUT useful_pct float8,
									OUT point_units float8, OUT range_units float8, OUT range_ideal float8, OUT correlation float8,
									OUT verdict text, OUT depth_max int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_clustering' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.col_clustering(regclass, float8) IS 'How well the rows of a sift_col index are ordered for its zone maps, one row per leaf: the units an equality or a range touches, the most units any value touches (depth_max), the correlation and a verdict (clustered, partial or scattered).';
CREATE FUNCTION sift.key_cut_decision(index regclass,
	OUT leaf text, OUT units float8, OUT sampled float8, OUT reads float8, OUT ideal float8, OUT growth_pct float8, OUT decision text, OUT reason text)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_col_key_cut_decision' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.key_cut_decision(regclass) IS 'What CREATE INDEX or REINDEX of a sift_col index would decide for key_cut now, from a sample of the heap: the key-shaped column, the units an equality on it reads (reads) against the units its rows fill (ideal), the predicted growth of the index when cut by it, the decision (cut, advise or none) and the reason. Changes nothing.';

-- Which physical order of a table (CLUSTER, or pg_repack --order-by on a heap table) would let the scans of its sift_col index skip the most units,
-- one row per index of rel (a table or an index; every index you may read when NULL). min_rows: the rows a query must save for the order to be
-- worth it (the default is what a scan reads in about a millisecond). It sorts the zones of the units virtually (docs/USAGE.md
-- "Should the table be ordered"): units_now is what an equality or range on the best leaf reads now (sift.col_clustering), units_after what a table
-- sorted by it reads (the units of the rows of one value, or of a window), saved_rows the rows a query no longer reads, churn_pct the share of the
-- rows modified since the last ANALYZE. verdict: recommended or not recommended, and reason says why (the table is small, already ordered, a
-- competing leaf that is ordered now would be scattered, the rows change faster than an order lasts, or the advice is switched off by the setting
-- sift.cluster_advice or the index option cluster_advice); command: the pg_repack call followed by the VACUUM (ANALYZE) a rewrite needs (it leaves
-- the visibility map empty, and a scan then checks the heap for every row), NULL when the table has no unique index for it. key: the order to ask
-- about, a list of leaves ('a, b'; NULL: the best leaf): order_by is then that list, units_now and units_after those of an equality on its first
-- leaf, and the leaves the list does not hold are the ones the sort can scatter. A reason ends with a note when under half of the heap pages are
-- all-visible (pg_class.relallvisible): run VACUUM (ANALYZE) first.
CREATE FUNCTION sift.cluster_advice(rel regclass DEFAULT NULL, min_rows int8 DEFAULT 100000, key text DEFAULT NULL,
									OUT tbl regclass, OUT index regclass, OUT order_by text, OUT units int8, OUT units_now float8,
									OUT units_after float8, OUT saved_rows int8, OUT churn_pct float8, OUT verdict text,
									OUT reason text, OUT command text)
	RETURNS SETOF record
	LANGUAGE plpgsql VOLATILE SET search_path = pg_catalog, pg_temp AS $fn$
DECLARE
	ix record;
	b record;
	n_rows int8;
	rpu float8;
	age float8;
	rate float8;
	second text;
	has_unique bool;
	kl text[];
	missing text;
	vm_note text;
	fq text;
	lj jsonb;
	cand text[];
	keyc text[];
	kb record;
	klost float8;
	klost_paths text;
	kcut text;
	has_kc bool;
	note text;
BEGIN
	IF key IS NOT NULL THEN
		kl := ARRAY(SELECT btrim(k) FROM unnest(string_to_array(key, ',')) k);
		IF array_length(kl, 1) IS NULL OR '' = ANY(kl) THEN
			RAISE EXCEPTION 'key must be a list of leaf names, such as ''a, b''' USING ERRCODE = 'invalid_parameter_value';
		END IF;
	END IF;
	FOR ix IN
		SELECT i.indexrelid::regclass AS idx, i.indrelid AS heap, c.reloptions, h.reltuples, h.relam, n.nspname, h.relname, h.relpages, h.relallvisible
		FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam
			JOIN pg_class h ON h.oid = i.indrelid JOIN pg_namespace n ON n.oid = h.relnamespace
		WHERE a.amname = 'sift_col' AND c.relkind = 'i' AND i.indisvalid AND (rel IS NULL OR rel IN (i.indrelid, i.indexrelid))
			AND has_table_privilege(i.indrelid, 'SELECT')
		ORDER BY i.indexrelid
	LOOP
		tbl := ix.heap; index := ix.idx; order_by := NULL; units := NULL; units_now := NULL; units_after := NULL; saved_rows := NULL;
		churn_pct := NULL; command := NULL; verdict := 'not recommended';
		IF NOT coalesce(current_setting('sift.cluster_advice', true)::bool, true) THEN
			reason := 'switched off by the setting sift.cluster_advice';
			RETURN NEXT; CONTINUE;
		END IF;
		IF NOT coalesce((SELECT split_part(o, '=', 2) FROM unnest(ix.reloptions) o WHERE o LIKE 'cluster_advice=%'), 'true')::bool THEN
			reason := 'switched off by the option cluster_advice of the index';
			RETURN NEXT; CONTINUE;
		END IF;
		SELECT i.rows, i.units INTO n_rows, units FROM sift.col_index_info(ix.idx) i;
		IF coalesce(units, 0) = 0 THEN
			reason := 'the index has no sealed unit yet';
			RETURN NEXT; CONTINUE;
		END IF;
		rpu := n_rows::float8 / units;
		fq := format('%I.%I', ix.nspname, ix.relname);
		vm_note := CASE WHEN ix.relpages > 0 AND ix.relallvisible < ix.relpages / 2.0
			THEN format('; only %s%% of the heap pages are all-visible: VACUUM (ANALYZE) %s first, or a scan checks the heap for every row of the others',
						round(100.0 * ix.relallvisible / ix.relpages), fq) ELSE '' END;
		IF kl IS NOT NULL THEN
			SELECT string_agg(k, ', ') INTO missing FROM unnest(kl) k
			WHERE k NOT IN (SELECT l.path FROM sift.col_clustering(ix.idx, 1) l WHERE l.point_units IS NOT NULL);
			IF missing IS NOT NULL THEN
				reason := format('the index has no integer, date or timestamp leaf named %s', missing);
				RETURN NEXT; CONTINUE;
			END IF;
		END IF;

		-- the leaves with what a sort by each would do: units read now and after, rows a value. An equality leaf (several rows a value, from the statistics
		-- of the column) is read at the units of its value; any other by a window of 1% of its span.
		SELECT jsonb_agg(to_jsonb(x)) INTO lj FROM (
			SELECT l.path, l.verdict AS lv, l.units AS total, s.rpv,
				   CASE WHEN s.rpv >= 2 THEN l.point_units ELSE l.range_units END AS now_u,
				   CASE WHEN s.rpv >= 2 THEN 1 + (s.rpv - 1) / rpu ELSE l.range_ideal END AS after_u
			FROM sift.col_clustering(ix.idx, 1) l
			CROSS JOIN LATERAL (
				SELECT n_rows / greatest(1, CASE WHEN st.n_distinct > 0 THEN st.n_distinct ELSE -st.n_distinct * ix.reltuples END) AS rpv
				FROM pg_stats st WHERE st.schemaname = ix.nspname AND st.tablename = ix.relname AND st.attname = l.path
				ORDER BY st.inherited LIMIT 1) s
			WHERE l.point_units IS NOT NULL
			UNION ALL
			SELECT l.path, l.verdict, l.units, NULL, l.range_units, l.range_ideal
			FROM sift.col_clustering(ix.idx, 1) l
			WHERE l.point_units IS NOT NULL
				AND NOT EXISTS (SELECT 1 FROM pg_stats st WHERE st.schemaname = ix.nspname AND st.tablename = ix.relname AND st.attname = l.path)) x;
		-- the best leaf: most units saved when the table is sorted by it. A leaf that is ordered now (clustered, partial) is what a
		-- sort by another, scattered leaf would give up.
		WITH leafs AS (SELECT * FROM jsonb_to_recordset(lj) AS l(path text, lv text, total int8, rpv float8, now_u float8, after_u float8)),
		best AS (SELECT * FROM leafs WHERE kl IS NULL OR path = kl[1] ORDER BY now_u - after_u DESC, path LIMIT 1)
		SELECT best.path, best.lv, best.now_u, best.after_u, best.rpv,
			   coalesce(sum((o.total - o.now_u) * rpu), 0) AS lost, string_agg(o.path, ', ' ORDER BY o.path) AS lost_paths
		INTO b
		FROM best LEFT JOIN leafs o ON o.path <> best.path AND (kl IS NULL OR o.path <> ALL (kl)) AND o.lv IN ('clustered', 'partial') AND best.lv = 'scattered'
		GROUP BY best.path, best.lv, best.now_u, best.after_u, best.rpv;
		IF b.path IS NULL THEN
			reason := 'the index has no integer, date or timestamp leaf with zones';
			RETURN NEXT; CONTINUE;
		END IF;

		order_by := b.path;
		-- the rows of one value fill several units: a date or timestamp leaf after it sorts the rows inside the value, so a window prunes there too
		IF kl IS NOT NULL THEN
			order_by := array_to_string(kl, ', ');
		ELSIF b.rpv > rpu THEN
			SELECT a.attname INTO second FROM pg_attribute a JOIN pg_type t ON t.oid = a.atttypid
			WHERE a.attrelid = ix.heap AND a.attnum > 0 AND NOT a.attisdropped AND a.attname <> b.path
				AND t.typname IN ('date', 'timestamp', 'timestamptz')
				AND a.attname IN (SELECT l.path FROM sift.col_clustering(ix.idx, 1) l WHERE l.point_units IS NOT NULL)
			ORDER BY a.attnum LIMIT 1;
			IF second IS NOT NULL THEN order_by := order_by || ', ' || second; END IF;
		END IF;
		units_now := b.now_u; units_after := b.after_u; saved_rows := ((b.now_u - b.after_u) * rpu)::int8;
		SELECT CASE WHEN ix.reltuples > 0 THEN 100.0 * s.n_mod_since_analyze / ix.reltuples END,
			   extract(epoch FROM now() - greatest(s.last_analyze, s.last_autoanalyze)) / 86400.0
		INTO churn_pct, age FROM pg_stat_all_tables s WHERE s.relid = ix.heap;
		rate := churn_pct / greatest(age, 1.0 / 24);

		SELECT EXISTS (SELECT 1 FROM pg_index u WHERE u.indrelid = ix.heap AND u.indisunique AND u.indisvalid AND u.indpred IS NULL) INTO has_unique;
		IF has_unique AND ix.relam = (SELECT oid FROM pg_am WHERE amname = 'heap') THEN
			command := format('pg_repack --table=%s --order-by=%L && psql -c %L', fq, order_by, 'VACUUM (ANALYZE) ' || fq);
		END IF;

		IF b.now_u <= b.after_u * 1.5 + 0.5 THEN
			reason := format('already ordered: %s reads %s units, a sorted table about %s', b.path, round(b.now_u::numeric, 1), round(b.after_u::numeric, 1));
		ELSIF units < 16 OR saved_rows < min_rows THEN
			reason := format('small: sorting by %s saves %s rows a query (under %s) in %s units', order_by, saved_rows, min_rows, units);
		ELSIF b.lost > 0 AND b.lost * 2 >= saved_rows THEN
			reason := format('competing leaves: sorting by %s would scatter %s, ordered now, which saves about %s rows a query against %s', order_by, b.lost_paths, round(b.lost), saved_rows);
		ELSIF rate >= 10 THEN
			reason := format('rows change too fast: %s%% modified since the last ANALYZE, %s%% a day; the order is lost faster than a repack restores it', round(churn_pct::numeric), round(rate::numeric));
		ELSE
			verdict := 'recommended';
			reason := format('an equality or range on %s reads %s units now and about %s sorted: %s rows fewer a query', b.path, round(b.now_u::numeric, 1), round(b.after_u::numeric, 1), saved_rows);
			IF b.lost > 0 THEN reason := reason || format('; it scatters %s, which saves about %s rows', b.lost_paths, round(b.lost)); END IF;
			IF command IS NULL THEN reason := reason || '; pg_repack needs a unique index (CLUSTER takes an exclusive lock)'; END IF;
			reason := reason || '; run VACUUM (ANALYZE) after the rewrite, which leaves the visibility map empty';
		END IF;
		-- the key a workload looks rows up by: a leaf of point_columns, point_tid_columns or key_columns that is not unique (or the first of key =>). key_cut keeps
		-- its units ordered through writes and merges, with no rewrite of the heap; it is only free when no leaf is ordered now (it would widen the zones of those).
		has_kc := EXISTS (SELECT 1 FROM unnest(ix.reloptions) o WHERE o ~ '^key_cut=(?!auto$)');
		IF NOT has_kc AND units >= 16 THEN
			cand := ARRAY(SELECT DISTINCT btrim(k) FROM unnest(ix.reloptions) o,
				LATERAL unnest(string_to_array(regexp_replace(split_part(o, '=', 2), '^auto:', ''), ',')) k
				WHERE o ~ '^(point_columns|point_tid_columns|key_columns)=' AND btrim(k) NOT IN ('', 'off', 'auto'));
			keyc := ARRAY(SELECT btrim(k) FROM unnest(ix.reloptions) o,
				LATERAL unnest(string_to_array(regexp_replace(split_part(o, '=', 2), '^auto:', ''), ',')) k WHERE o ~ '^key_columns=');
			SELECT l.path, l.now_u, l.after_u, l.rpv INTO kb
			FROM jsonb_to_recordset(lj) AS l(path text, lv text, total int8, rpv float8, now_u float8, after_u float8)
			WHERE l.path = ANY(CASE WHEN kl IS NOT NULL THEN kl[1:1] ELSE cand END) AND (l.rpv IS NULL OR l.rpv >= 2)
			ORDER BY l.now_u - l.after_u DESC, l.path LIMIT 1;
			note := NULL;
			IF kb.path IS NOT NULL AND (kb.now_u - kb.after_u) * rpu >= min_rows AND kb.now_u > kb.after_u * 1.5 + 0.5 THEN
				SELECT coalesce(sum((o.total - o.now_u) * rpu), 0), string_agg(o.path, ', ' ORDER BY o.path) INTO klost, klost_paths
				FROM jsonb_to_recordset(lj) AS o(path text, lv text, total int8, rpv float8, now_u float8, after_u float8)
				WHERE o.path <> kb.path AND (kl IS NULL OR o.path <> ALL (kl)) AND o.lv IN ('clustered', 'partial');
				kcut := coalesce(array_to_string(kl, ', '), kb.path);
				IF klost = 0 THEN
					note := format('key_cut = %L cuts the units by %s: %s reads about %s units, not %s, and keeps that through writes and merges without a rewrite of the table, and no leaf that is ordered now is widened (ALTER INDEX %s SET (key_cut = %L), then REINDEX INDEX %s; the index grows by the unit address lists)',
								   kcut, kcut, kb.path, round(kb.after_u::numeric, 1), round(kb.now_u::numeric, 1), ix.idx, kcut, ix.idx);
					IF kb.rpv IS NOT NULL AND kb.path <> ALL (keyc) AND n_rows / kb.rpv > 256 THEN
						note := note || format('; key_columns = %L keeps the keys of each unit, which lets a lookup skip the units that a few foreign rows widen', kb.path);
					END IF;
				ELSIF verdict = 'recommended' THEN
					note := format('key_cut = %L would widen %s, which are ordered now', kcut, klost_paths);
				END IF;
			ELSIF kb.path IS NULL AND b.lost = 0 AND (b.now_u - b.after_u) * rpu >= min_rows AND b.now_u > b.after_u * 1.5 + 0.5 THEN
				note := 'to keep a key ordered through writes without a rewrite, set key_cut to the column the queries look rows up by (up to four, most significant first; a column of point_columns or key_columns that is not unique is found by this function), and check it with key => ''col''';
			END IF;
			IF note IS NOT NULL THEN reason := reason || '; ' || note; END IF;
		END IF;
		reason := reason || vm_note;
		IF verdict <> 'recommended' THEN command := NULL; END IF;
		RETURN NEXT;
	END LOOP;
END
$fn$;
COMMENT ON FUNCTION sift.cluster_advice(regclass, int8, text) IS 'Which physical order of a table (CLUSTER, pg_repack --order-by) would let the scans of its sift_col index skip the most units, one row per index: units read now and after, rows saved, churn, a verdict with its reason, and the pg_repack call with the VACUUM (ANALYZE) that follows; key asks about your own list of leaves.';

-- What an update to 3.3 leaves to do, one row per command, the ones that cost least first (a REINDEX CONCURRENTLY before a VACUUM FULL, and what queries
-- need before what they only gain from). kind: reindex or rewrite; object: the index or table; reason: what the command changes; command: the statement;
-- required: queries miss rows until it runs (a word over 255 bytes is stored as its start and a hash since 3.3, and a text index of an older release holds
-- it cut at 255 bytes; the index does not say whether it holds one, so every text index except analyzer = keyword is listed, and stays listed after its
-- REINDEX). Otherwise it is a gain: a sift_col index with an option (cube_minmax and superunit, on unless the index says otherwise; JSON paths, rollup_dims, key_columns, order_columns, order_array_columns, hll_columns) that only the units written after
-- it follow, set on an index whose older units lack the section; a sift_table (it cannot be reindexed) whose segments are below the column format its
-- schema is written in now (calendar cells for date and timestamp columns, minor 8; complete JSON kinds for declared paths, minor 6). A VACUUM FULL takes
-- an ACCESS EXCLUSIVE lock and rewrites the table in the format of sift.col_format_minor: schedule it. Reads the catalogs and the metapages, changes
-- nothing, lists the indexes and tables the caller owns or may maintain (the privilege REINDEX and VACUUM FULL ask for; all of them for a superuser), and gives up on a relation that is locked at once (lock_timeout) or that this library refuses (sift.upgrade_check reports those); a library
-- that cannot be loaded is an error. ALTER EXTENSION pg_sift UPDATE to 3.3 prints this list.
CREATE FUNCTION sift.upgrade_commands(OUT kind text, OUT object regclass, OUT reason text, OUT command text, OUT required bool)
	RETURNS SETOF record
	LANGUAGE plpgsql SET lock_timeout = '100ms' AS $fn$
DECLARE
	r record;
	lo int;
	why text;
BEGIN
	kind := 'reindex'; required := true;
	FOR r IN SELECT c.oid AS relid, format('%I.%I', n.nspname, c.relname) AS name
			 FROM pg_class c JOIN pg_am am ON am.oid = c.relam JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_index i ON i.indexrelid = c.oid
			 WHERE am.amname = 'sift' AND c.relkind = 'i' AND has_table_privilege(i.indrelid, 'MAINTAIN') AND 'analyzer=keyword' <> ALL (coalesce(c.reloptions, '{}')) ORDER BY n.nspname, c.relname
	LOOP
		object := r.relid; command := format('REINDEX INDEX CONCURRENTLY %s;', r.name);
		reason := 'long words (over 255 bytes) are found only after REINDEX: an index built before 3.3 holds such a word cut at 255 bytes, so a query for it finds nothing';
		RETURN NEXT;
	END LOOP;
	required := false;
	FOR r IN SELECT c.oid AS relid, c.reloptions AS opts, format('%I.%I', n.nspname, c.relname) AS name
			 FROM pg_class c JOIN pg_am am ON am.oid = c.relam JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_index i ON i.indexrelid = c.oid
			 WHERE am.amname = 'sift_col' AND c.relkind = 'i' AND has_table_privilege(i.indrelid, 'MAINTAIN') ORDER BY n.nspname, c.relname
	LOOP
		BEGIN
			SELECT min((to_jsonb(s)->>'format')::int) INTO lo FROM sift.col_segments(r.relid) s;
		EXCEPTION WHEN OTHERS THEN
			IF SQLSTATE IN ('42883', '58P01', '55P03') THEN RAISE; END IF;
			CONTINUE;
		END;
		CONTINUE WHEN lo IS NULL;
		-- a segment is minor 9, 10, 13, 14, 15 or 20 when some unit holds the section (for 14, when the segment holds a point directory), so a lower minor means none does (docs/COL.md 2.13, 2.14, 2.17)
		-- cube_minmax and superunit are on unless the index says otherwise (dflt): the units written before 3.3 lack their sections
		SELECT string_agg(format('%s with column format %s: %s', v.what, lo, v.why), '; ' ORDER BY v.minor) INTO why FROM (VALUES
				('^paths=', 6, 'JSON paths', false,
				 'segments before format 6 may lack the kinds of a JSON null or of a type; a predicate that needs them uses the normal plan, and a REINDEX writes them'),
				('^cube_minmax=(false|off)$', 7, 'cube_minmax', true,
				 'the units written before the option scan; a REINDEX writes the minimum and maximum of every unit'),
				('^rollup_dims=.', 9, 'rollup_dims', false,
				 'the units written before the option have no rollup of their rows and are scanned (nor has a unit over the caps of the rollup, which a REINDEX does not change); a REINDEX writes it where it fits'),
				('^key_columns=.', 10, 'key_columns', false,
				 'the units written before the option have no keys of the column (a summary, or a key list for a count(DISTINCT)), so an equality or a distinct count reads them whole (nor has a unit whose keys would take more than 3% of its bytes, which a REINDEX does not change); a REINDEX writes them where they fit'),
				('^superunit=0$', 12, 'superunit', true,
				 'the units written before the option have no summary of a group of units, so a scan of the whole index reads and folds every unit; a REINDEX writes it'),
				('^order_columns=.', 13, 'order_columns', false,
				 'the units written before the option have no sorted run, so an ORDER BY .. LIMIT on the column reads them whole; a REINDEX writes it'),
				('^point_columns=.', 14, 'point_columns', false,
				 'the segments written before the option have no point directory, so an equality or IN list on the column reads their units (zones and summaries cut most); a REINDEX writes it'),
				('^point_tid_columns=.', 16, 'point_tid_columns', false,
				 'the segments written before the option list the rows of the column by their number in the segment, so an equality or IN list on the column reads the units of the rows it finds; a REINDEX lists their heap addresses'),
				('^key_cut=(?!off$|auto$)', 17, 'key_cut', false,
				 'the units written before the option are cut by superblock, so a lookup or a range of the key reads units from every part of the heap; a REINDEX cuts them by the key'),
				('^hll_columns=.', 20, 'hll_columns', false,
				 'the units written before the option have no sketch of the column, so sift.approx_count_distinct reads their rows; a REINDEX writes it'),
				('^order_array_columns=(?!off$|auto$)', 21, 'order_array_columns', false,
				 'the segments written before the option have no global order of the columns, so an ORDER BY .. LIMIT above 32 rows on one reads their units; a REINDEX writes it')
			 ) v(pat, minor, what, dflt, why)
			 WHERE lo < v.minor AND EXISTS (SELECT 1 FROM unnest(r.opts) opt WHERE opt ~ v.pat) <> v.dflt;
		CONTINUE WHEN why IS NULL;
		object := r.relid; reason := why; command := format('REINDEX INDEX CONCURRENTLY %s;', r.name);
		RETURN NEXT;
	END LOOP;
	kind := 'rewrite';
	FOR r IN SELECT c.oid AS relid, format('%I.%I', n.nspname, c.relname) AS name,
					EXISTS (SELECT 1 FROM pg_attribute a WHERE a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped
							AND a.atttypid IN ('date'::regtype, 'timestamp'::regtype, 'timestamptz'::regtype)) AS calendar,
					EXISTS (SELECT 1 FROM sift.table_paths_maintained p WHERE p.relid = c.oid) AS paths
			 FROM pg_class c JOIN pg_am am ON am.oid = c.relam JOIN pg_namespace n ON n.oid = c.relnamespace
			 WHERE am.amname = 'sift_table' AND c.relkind IN ('r', 'm') AND has_table_privilege(c.oid, 'MAINTAIN') ORDER BY n.nspname, c.relname
	LOOP
		BEGIN
			SELECT min(s.format) INTO lo FROM sift.table_segments(r.relid) s WHERE s.kind = 'columns';
		EXCEPTION WHEN OTHERS THEN
			IF SQLSTATE IN ('42883', '58P01', '55P03') THEN RAISE; END IF;
			CONTINUE;
		END;
		-- the segments of a table with declared paths are never minor 8 (src/col/seg_write.c)
		why := concat_ws('; ',
				CASE WHEN r.calendar AND NOT r.paths AND lo < 8 THEN
					format('column format %s: the segments have no calendar cells of the date and timestamp columns, so a GROUP BY on a day or a month of them reads the rows', lo) END,
				CASE WHEN r.paths AND lo < 6 THEN
					format('column format %s: the segments may lack the kinds of a JSON null or of a type of the declared paths, and a predicate that needs them uses the normal plan', lo) END);
		CONTINUE WHEN why = '';
		object := r.relid; reason := why || '. It takes an ACCESS EXCLUSIVE lock and rewrites the table in the format of sift.col_format_minor: schedule it';
		command := format('VACUUM FULL %s;', r.name);
		RETURN NEXT;
	END LOOP;
END
$fn$;
COMMENT ON FUNCTION sift.upgrade_commands() IS 'What an update leaves to do for the indexes and tables the caller owns or may maintain, one row per command, the cheapest first: kind (reindex, rewrite), the index or table, the reason and the statement; required when queries miss rows until it runs. Changes nothing.';

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
-- scan_cost and normal_cost = the costs of the path that was built and of the cheapest normal path it was compared with, both of the grouping stage (the
-- total before an ORDER BY or a LIMIT above it: PostgreSQL chooses the plan of the statement by the cost of its top node); NULL when there was no such path.
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

CREATE VIEW sift.col_rollup_state AS SELECT * FROM sift.col_rollup_counters();
CREATE VIEW sift.col_rollup_requests AS SELECT * FROM sift.col_rollup_queue();
-- the functions they list are open to every role with USAGE on the schema; the views are too (they were readable by superusers only)
GRANT SELECT ON sift.col_rollup_state, sift.col_rollup_requests TO PUBLIC;

-- Facet counts over a search (docs/FIELDS-FACETS.md): terms, range, histogram and date_histogram facets of any column type, several in one
-- statement, with the facets of a multi-select (drill-sideways) and a top N with an "other" count. sift.facet_sql builds the SQL statement
-- (one `GROUP BY` scan per facet, the search as one `key @@> query` conjunct of each; the ranges of a facet list share one scan of
-- FILTER aggregates) that sift.facet_counts runs: with a sift_col index on the columns each scan runs as Custom Scan (SiftAgg) with the
-- search as the row selection, without one it is an ordinary plan and still exact.

-- Raises when a gap-filled histogram would return more than 10000 buckets; returns its argument otherwise.
CREATE FUNCTION sift.facet_cap(buckets int8) RETURNS int8
	LANGUAGE plpgsql VOLATILE PARALLEL SAFE AS $fn$
BEGIN
	IF buckets > 10000 THEN
		RAISE EXCEPTION 'a facet with "fill" would return more than 10000 buckets' USING ERRCODE = 'program_limit_exceeded',
			HINT = 'Use a larger interval or unit, or leave "fill" off.';
	END IF;
	RETURN buckets;
END $fn$;

-- One facet of sift.facet_sql: its name, the group key (NULL for a range: its buckets are the FILTER aggregates faggs), its own filter (the
-- "selected" values and "filter" of the spec, NULL if none), the array its key comes from unnesting (NULL: none) and the SQL that shapes the
-- rows of its scan into (facet_no, seq, facet, kind, value, lo, hi, count); {cte} and {k} stand for the scan and its key column.
CREATE FUNCTION sift.facet_part(spec jsonb, no int,
								OUT fname text, OUT fkey text, OUT fown text, OUT fsrc text, OUT faggs text, OUT fshape text)
	LANGUAGE plpgsql STABLE AS $fn$
DECLARE
	s		jsonb := CASE WHEN jsonb_typeof(spec) = 'string' THEN jsonb_build_object('expr', spec #>> '{}') ELSE spec END;
	ex		text;
	t		text;
	sel		text;
	lits	text;
	ord		text;
	sz		int;
	mx		text;
	ranked	text;
	rng		text;
	itv		numeric;
	off		numeric;
	unit	text;
	step	text;
	edges	text[];
	n		int;
	conds	text[];
	lo		text;
	hi		text;
	k		text := '{k}';
	c		text := '{cte}';
	miss	text := '';
	arr		text[];
BEGIN
	IF jsonb_typeof(s) IS DISTINCT FROM 'object' THEN
		RAISE EXCEPTION 'facet %: a facet is a column expression or an object', no USING ERRCODE = 'invalid_parameter_value';
	END IF;
	ex := coalesce(s->>'expr', s->>'name');
	IF ex IS NULL THEN
		RAISE EXCEPTION 'facet %: "expr" or "name" is required', no USING ERRCODE = 'invalid_parameter_value';
	END IF;
	fname := coalesce(s->>'name', ex);
	t := coalesce(s->>'type', CASE WHEN s ? 'edges' THEN 'range' WHEN s ? 'unit' THEN 'date_histogram'
								   WHEN s ? 'interval' THEN 'histogram' ELSE 'terms' END);
	IF t NOT IN ('terms', 'range', 'histogram', 'date_histogram') THEN
		RAISE EXCEPTION 'facet %: unknown type "%" (terms, range, histogram, date_histogram)', no, t USING ERRCODE = 'invalid_parameter_value';
	END IF;
	IF coalesce((s->>'array')::bool, false) AND t <> 'terms' THEN
		RAISE EXCEPTION 'facet %: "array" is for terms facets', no USING ERRCODE = 'invalid_parameter_value';
	END IF;
	lits := (SELECT string_agg(quote_literal(v), ', ') FROM jsonb_array_elements_text(coalesce(s->'selected', '[]')) v);
	IF coalesce((s->>'missing')::bool, false) AND t <> 'range' THEN
		miss := format(' UNION ALL SELECT %s, 4611686018427387905, %L, ''missing'', NULL::text, NULL::text, NULL::text, c FROM %s WHERE %s IS NULL',
					   no, fname, c, k);
	END IF;

	IF t = 'terms' THEN
		fsrc := CASE WHEN coalesce((s->>'array')::bool, false) THEN ex END;
		fkey := CASE WHEN fsrc IS NOT NULL THEN 'sift_facet_u.sift_facet_k' ELSE '(' || ex || ')' END;
		sel := CASE WHEN lits IS NULL THEN NULL
					WHEN fsrc IS NOT NULL THEN format('(%s)::text[] && ARRAY[%s]::text[]', ex, lits)
					ELSE format('(%s) IN (%s)', ex, lits) END;
		sz := (s->>'size')::int;
		ord := lower(coalesce(s->>'order', 'count'));
		ord := CASE ord WHEN 'count' THEN 'c DESC, ' || k WHEN 'count desc' THEN 'c DESC, ' || k WHEN 'count asc' THEN 'c, ' || k
						WHEN 'value' THEN k WHEN 'value asc' THEN k WHEN 'value desc' THEN k || ' DESC' END;
		IF ord IS NULL THEN
			RAISE EXCEPTION 'facet %: "order" is count, count asc, value or value desc', no USING ERRCODE = 'invalid_parameter_value';
		END IF;
		ranked := format('(SELECT %s k, c, row_number() OVER (ORDER BY %s) rn FROM %s WHERE %s IS NOT NULL) t', k, ord, c, k);
		fshape := format('SELECT %s, rn, %L, ''value'', k::text, NULL::text, NULL::text, c FROM %s%s', no, fname, ranked,
						 CASE WHEN sz IS NULL THEN '' ELSE format(' WHERE rn <= %s', sz) END);
		IF sz IS NOT NULL THEN
			fshape := fshape || format(' UNION ALL SELECT %s, 4611686018427387904, %L, ''other'', NULL::text, count(*)::text, NULL::text, sum(c)::int8 FROM %s WHERE rn > %s HAVING count(*) > 0',
									   no, fname, ranked, sz);
		END IF;
	ELSIF t = 'range' THEN
		edges := ARRAY(SELECT quote_literal(v) FROM jsonb_array_elements_text(s->'edges') v);
		n := coalesce(array_length(edges, 1), 0);
		IF n = 0 THEN
			RAISE EXCEPTION 'facet %: "edges" must list at least one bound', no USING ERRCODE = 'invalid_parameter_value';
		END IF;
		-- bucket i is [edges[i], edges[i + 1]); the first and the last are open: the buckets are FILTER aggregates of one scan without keys
		conds := ARRAY(SELECT concat_ws(' AND ', CASE WHEN i > 1 THEN format('(%s) >= %s', ex, edges[i - 1]) END,
												 CASE WHEN i <= n THEN format('(%s) < %s', ex, edges[i]) END) FROM generate_series(1, n + 1) i);
		faggs := (SELECT string_agg(format('count(*) FILTER (WHERE %s) a%s_%s', conds[i], no, i - 1), ', ' ORDER BY i) FROM generate_series(1, n + 1) i);
		IF coalesce((s->>'missing')::bool, false) THEN
			faggs := faggs || format(', count(*) FILTER (WHERE (%s) IS NULL) a%s_m', ex, no);
			miss := format(' UNION ALL SELECT %s, 4611686018427387905, %L, ''missing'', NULL::text, NULL::text, NULL::text, a%s_m FROM %s WHERE a%s_m > 0',
						   no, fname, no, c, no);
		END IF;
		arr := ARRAY(SELECT format('a%s_%s', no, i - 1) FROM generate_series(1, n + 1) i);
		rng := (SELECT string_agg(format('(%s, %L, %s, %s)', i - 1,
							'[' || CASE WHEN i = 1 THEN '' ELSE trim(both '''' from edges[i - 1]) END || ',' ||
								   CASE WHEN i = n + 1 THEN '' ELSE trim(both '''' from edges[i]) END || ')',
							CASE WHEN i = 1 THEN 'NULL::text' ELSE edges[i - 1] || '::text' END,
							CASE WHEN i = n + 1 THEN 'NULL::text' ELSE edges[i] || '::text' END), ', ' ORDER BY i)
				FROM generate_series(1, n + 1) i);
		fshape := format('SELECT %s, b.i, %L, ''bucket'', b.label, b.lo, b.hi, (ARRAY[%s])[b.i + 1] FROM (VALUES %s) b(i, label, lo, hi) CROSS JOIN %s',
						 no, fname, array_to_string(arr, ', '), rng, c);
		-- the buckets of the selection as comparisons the column scan can take
		sel := CASE WHEN lits IS NULL THEN NULL ELSE
			(SELECT '(' || string_agg('(' || conds[i] || ')', ' OR ') || ')' FROM generate_series(1, n + 1) i
			 WHERE (i - 1)::text IN (SELECT jsonb_array_elements_text(s->'selected'))) END;
	ELSIF t = 'histogram' THEN
		itv := (s->>'interval')::numeric;
		off := coalesce((s->>'offset')::numeric, 0);
		IF itv IS NULL OR itv <= 0 THEN
			RAISE EXCEPTION 'facet %: "interval" must be a positive number', no USING ERRCODE = 'invalid_parameter_value';
		END IF;
		fkey := format('floor(((%s)::numeric - %s) / %s)', ex, off, itv);
		sel := CASE WHEN lits IS NULL THEN NULL ELSE (SELECT '(' || string_agg(format('(%s) >= %s AND (%s) < %s', ex, v::numeric, ex, v::numeric + itv), ' OR ') || ')'
													 FROM jsonb_array_elements_text(s->'selected') v) END;
		lo := format('k * %s + %s', itv, off);
		hi := format('(k + 1) * %s + %s', itv, off);
		IF coalesce((s->>'fill')::bool, false) THEN
			mx := format('(SELECT s.k, coalesce(r.c, 0) c FROM (SELECT generate_series(a.mn, a.mn + sift.facet_cap((a.mx - a.mn)::int8)) k '
						 'FROM (SELECT min(%s) mn, max(%s) mx FROM %s WHERE %s IS NOT NULL) a WHERE a.mn IS NOT NULL) s '
						 'LEFT JOIN (SELECT %s k, c FROM %s) r ON r.k = s.k) d', k, k, c, k, k, c);
		ELSE
			mx := format('(SELECT %s k, c FROM %s WHERE %s IS NOT NULL) d', k, c, k);
		END IF;
		fshape := format('SELECT %s, row_number() OVER (ORDER BY k), %L, ''bucket'', (%s)::text, (%s)::text, (%s)::text, c FROM %s',
						 no, fname, lo, lo, hi, mx);
	ELSE
		unit := lower(s->>'unit');
		step := CASE unit WHEN 'second' THEN '1 second' WHEN 'minute' THEN '1 minute' WHEN 'hour' THEN '1 hour' WHEN 'day' THEN '1 day'
						  WHEN 'week' THEN '1 week' WHEN 'month' THEN '1 month' WHEN 'quarter' THEN '3 months' WHEN 'year' THEN '1 year' END;
		IF step IS NULL THEN
			RAISE EXCEPTION 'facet %: "unit" is second, minute, hour, day, week, month, quarter or year', no USING ERRCODE = 'invalid_parameter_value';
		END IF;
		fkey := format('date_trunc(%L, (%s)::timestamp)', unit, ex);
		sel := CASE WHEN lits IS NULL THEN NULL ELSE (SELECT '(' || string_agg(format('(%s)::timestamp >= %L::timestamp AND (%s)::timestamp < %L::timestamp + interval %L',
													ex, v, ex, v, step), ' OR ') || ')' FROM jsonb_array_elements_text(s->'selected') v) END;
		IF coalesce((s->>'fill')::bool, false) THEN
			mx := format('(SELECT s.k, coalesce(r.c, 0) c FROM (SELECT a.k, sift.facet_cap(count(*) OVER ()) FROM (SELECT generate_series(a.mn, a.mx, interval %L) k '
						 'FROM (SELECT min(%s) mn, max(%s) mx FROM %s WHERE %s IS NOT NULL) a WHERE a.mn IS NOT NULL LIMIT 10001) a) s '
						 'LEFT JOIN (SELECT %s k, c FROM %s) r ON r.k = s.k) d', step, k, k, c, k, k, c);
		ELSE
			mx := format('(SELECT %s k, c FROM %s WHERE %s IS NOT NULL) d', k, c, k);
		END IF;
		fshape := format('SELECT %s, row_number() OVER (ORDER BY k), %L, ''bucket'', k::text, k::text, (k + interval %L)::text, c FROM %s',
						 no, fname, step, mx);
	END IF;

	IF s ? 'filter' THEN
		sel := coalesce('(' || sel || ') AND ', '') || '(' || (s->>'filter') || ')';
	END IF;
	fown := sel;
	fshape := fshape || miss;
END $fn$;

CREATE FUNCTION sift.facet_sql(tbl regclass, key text, query text, facets jsonb,
							   filter text DEFAULT NULL, hits bool DEFAULT false)
	RETURNS text LANGUAGE plpgsql STABLE AS $fn$
DECLARE
	n		int;
	p		record;
	i		int;
	j		int;
	fkey	text[] := '{}';
	fown	text[] := '{}';
	fsrc	text[] := '{}';
	faggs	text[] := '{}';
	fshape	text[] := '{}';
	common	text[] := '{}';
	w		text;
	rw		text[] := '{}';
	ra		text[] := '{}';
	ctes	text[] := '{}';
	shapes	text[] := '{}';
	scan	int[] := '{}';
BEGIN
	IF jsonb_typeof(facets) IS DISTINCT FROM 'array' OR jsonb_array_length(facets) = 0 THEN
		RAISE EXCEPTION 'facets must be a non-empty JSON array' USING ERRCODE = 'invalid_parameter_value';
	END IF;
	n := jsonb_array_length(facets);
	FOR i IN 1..n LOOP
		SELECT * INTO p FROM sift.facet_part(facets -> (i - 1), i);
		fkey := fkey || coalesce(p.fkey, ''); fown := fown || coalesce(p.fown, ''); fsrc := fsrc || coalesce(p.fsrc, '');
		faggs := faggs || coalesce(p.faggs, ''); fshape := fshape || p.fshape;
	END LOOP;
	IF query IS NOT NULL THEN
		IF key IS NULL THEN
			RAISE EXCEPTION 'key: the indexed expression of the search is required with a query' USING ERRCODE = 'null_value_not_allowed';
		END IF;
		common := common || format('(%s) @@> %L', key, query);
	END IF;
	IF filter IS NOT NULL THEN common := common || ('(' || filter || ')'); END IF;

	-- a facet counts what every other facet has selected but not its own selection: its WHERE is the search and the filter with the
	-- selections of the others. One scan per facet; the ranges with the same WHERE are the aggregates of one scan
	FOR i IN 1..n LOOP
		w := coalesce(nullif(array_to_string(common || ARRAY(SELECT o FROM unnest(fown) WITH ORDINALITY u(o, k) WHERE k <> i AND o <> ''), ' AND '), ''), 'true');
		IF fkey[i] <> '' THEN
			ctes := ctes || format('s%s AS MATERIALIZED (SELECT %s k%s, count(*) c FROM %s%s WHERE %s GROUP BY %s)', i, fkey[i], i, tbl,
				CASE WHEN fsrc[i] <> '' THEN format(' CROSS JOIN LATERAL (SELECT DISTINCT e AS sift_facet_k FROM unnest(%s) e) sift_facet_u', fsrc[i]) ELSE '' END,
				w, fkey[i]);
			scan := scan || i;
		ELSE
			j := NULL;
			SELECT x INTO j FROM generate_series(1, coalesce(array_length(rw, 1), 0)) x WHERE rw[x] = w;
			IF j IS NULL THEN rw := rw || w; ra := ra || ''::text; j := array_length(rw, 1); END IF;
			ra[j] := concat_ws(', ', nullif(ra[j], ''), faggs[i]);
			scan := scan || -j;
		END IF;
	END LOOP;
	FOR j IN 1..coalesce(array_length(rw, 1), 0) LOOP
		ctes := ctes || format('r%s AS MATERIALIZED (SELECT %s FROM %s WHERE %s)', j, ra[j], tbl, rw[j]);
	END LOOP;
	FOR i IN 1..n LOOP
		shapes := shapes || replace(replace(fshape[i], '{cte}', CASE WHEN scan[i] > 0 THEN 's' || scan[i] ELSE 'r' || -scan[i] END), '{k}', 'k' || i);
	END LOOP;
	IF hits THEN
		shapes := shapes || format('SELECT 0, 0::int8, NULL::text, ''hits'', NULL::text, NULL::text, NULL::text, count(*) FROM %s WHERE %s',
								   tbl, coalesce(nullif(array_to_string(common || ARRAY(SELECT o FROM unnest(fown) o WHERE o <> ''), ' AND '), ''), 'true'));
	END IF;
	RETURN 'WITH ' || array_to_string(ctes, E',\n     ') || E'\nSELECT facet, kind, value, lo, hi, c FROM (\n  ' ||
		   array_to_string(shapes, E'\n  UNION ALL\n  ') || E'\n) x(no, seq, facet, kind, value, lo, hi, c) ORDER BY no, seq';
END $fn$;

-- The counts of the facets of a search: one row per bucket, in the order of the facets. facets is a JSON array whose elements are a column
-- expression (a terms facet) or an object:
--   {"name": "brand", "expr": "brand", "type": "terms", "size": 10, "order": "count", "missing": true, "array": false}
--   {"name": "price", "expr": "price", "type": "range", "edges": [100, 300]}              buckets [,100) [100,300) [300,)
--   {"name": "price", "expr": "price", "type": "histogram", "interval": 50, "offset": 0, "fill": true}
--   {"name": "added", "expr": "added", "type": "date_histogram", "unit": "month", "fill": true}
-- "selected" (values; bucket numbers of a range; the bucket starts of a histogram) and "filter" (a SQL condition) are what the user chose in that
-- facet: they filter every other facet but not this one. key is the expression the sift index is on and query the SiftQL search (NULL: no
-- search), filter a SQL condition for every facet, hits adds the row (facet NULL, kind 'hits') counting what every selection leaves.
-- kind: value (terms), bucket, other (the values past size: lo is how many, count the sum of their counts), missing (NULL values) and hits.
CREATE FUNCTION sift.facet_counts(tbl regclass, key text, query text, facets jsonb,
								  filter text DEFAULT NULL, hits bool DEFAULT false,
								  OUT facet text, OUT kind text, OUT value text, OUT lo text, OUT hi text, OUT count int8)
	RETURNS SETOF record LANGUAGE plpgsql STABLE AS $fn$
BEGIN
	RETURN QUERY EXECUTE sift.facet_sql(tbl, key, query, facets, filter, hits);
END $fn$;

-- Fields (docs/FIELDS-FACETS.md): the sections of an indexed text, joined here so that no query or index writes chr(29): CREATE INDEX ... USING sift
-- (sift.sections(title, body)) WITH (fields = 'title,body'); a NULL section is empty, and the same expression is the left side of @@>.
CREATE FUNCTION sift.sections(VARIADIC parts text[]) RETURNS text
	LANGUAGE sql IMMUTABLE PARALLEL SAFE RETURN array_to_string(parts, chr(29), '');

-- The query with the weights of its fields: every word, prefix, pattern and fuzzy term outside a name: qualifier is searched in each of the fields
-- of `fields` ("title^3, body^1, tags^0.5", a missing weight is 1) with its boost times the weight of the field, so a row scores the sum
-- of its fields' scores (the match set is that of the unweighted query on those fields). Phrases, regular expressions and the terms of a NOT are not
-- scoped to a field. With an index its fields are checked.
CREATE FUNCTION sift.field_query(query text, fields text, index regclass DEFAULT NULL) RETURNS text
	AS 'MODULE_PATHNAME', 'sift_field_query' LANGUAGE C STABLE PARALLEL SAFE;

-- Paths of a jsonb column of a sift_table (docs/COL-JSONB-31.md section 4): a table access method has no options, so the declaration is a
-- row here, by table and column name, in the grammar of the `paths` option of sift_col for that one column. A change applies to the segments
-- written afterwards (promotion, merge, VACUUM FULL); until every segment has the paths a query reads the column as before.
CREATE TABLE sift.table_paths (relid regclass NOT NULL, attname name NOT NULL, spec text NOT NULL, PRIMARY KEY (relid, attname));
SELECT pg_catalog.pg_extension_config_dump('sift.table_paths', '');
-- The declared paths of the sift_tables the caller may maintain (owner or MAINTAIN): sift.table_paths is for superusers and the extension's owner, and an
-- owner could not read the paths of its own table. The view runs with the rights of its owner, so a condition of a query is checked after the filter.
CREATE VIEW sift.table_paths_maintained WITH (security_barrier = true) AS
	SELECT p.relid, p.attname, p.spec FROM sift.table_paths p WHERE has_table_privilege(p.relid, 'MAINTAIN');
COMMENT ON VIEW sift.table_paths_maintained IS 'The declared JSON paths of the sift_tables the caller owns or may maintain (relid, attname, spec); sift.table_paths lists every table and is for superusers.';
GRANT SELECT ON sift.table_paths_maintained TO PUBLIC;

-- Checks that the table can hold the paths and rebuilds its layout (C).
CREATE FUNCTION sift.table_paths_check(tbl regclass, col name, spec text) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_table_paths_check' LANGUAGE C VOLATILE;

-- Declares the paths of column col of the sift_table tbl; an empty or NULL spec removes the declaration. The check above requires the owner of
-- the table (the session's role, which SECURITY DEFINER does not change), and the function writes the table as the extension's owner: a role
-- has no privilege on sift.table_paths.
CREATE FUNCTION sift.table_set_paths(tbl regclass, col name, spec text) RETURNS void
	LANGUAGE plpgsql VOLATILE SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $fn$
BEGIN
	PERFORM sift.table_paths_check(tbl, col, spec);
	IF coalesce(spec, '') = '' THEN
		DELETE FROM sift.table_paths WHERE relid = tbl AND attname = col;
	ELSE
		INSERT INTO sift.table_paths VALUES (tbl, col, spec) ON CONFLICT (relid, attname) DO UPDATE SET spec = EXCLUDED.spec;
		IF EXISTS (SELECT 1 FROM sift.table_segments(tbl) WHERE kind = 'columns') THEN
			RAISE NOTICE 'the column groups of "%" written so far may keep "%" as one leaf: its paths are used once every group has their leaves (VACUUM FULL rewrites them)', tbl, col;
		END IF;
	END IF;
END
$fn$;

-- The declaration goes with the table. The trigger fires for every DROP of every role, so it runs as the extension's owner, who owns
-- sift.table_paths: as the dropping role it failed with "permission denied for schema sift" (3.2.0, 3.2.1).
CREATE FUNCTION sift.table_paths_drop() RETURNS event_trigger
	LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $fn$
BEGIN
	DELETE FROM sift.table_paths p USING pg_event_trigger_dropped_objects() d
		WHERE d.classid = 'pg_class'::regclass AND d.objsubid = 0 AND p.relid = d.objid;
END
$fn$;

CREATE EVENT TRIGGER sift_table_paths_drop ON sql_drop EXECUTE FUNCTION sift.table_paths_drop();

-- A column added to a sift_table past the 127 it holds (or a column of declared paths renamed or retyped into the excess) fails the ALTER
-- TABLE, as CREATE TABLE does, instead of the next write.
CREATE FUNCTION sift.table_ddl_check() RETURNS event_trigger
	AS 'MODULE_PATHNAME', 'sift_table_ddl_check' LANGUAGE C;

CREATE EVENT TRIGGER sift_table_ddl_check ON ddl_command_end WHEN TAG IN ('ALTER TABLE') EXECUTE FUNCTION sift.table_ddl_check();

-- pg_repack copies a table into a heap table and swaps the files: its trigger repack.repack_trigger is refused on a sift_table, which CLUSTER or
-- VACUUM FULL rewrite.
CREATE EVENT TRIGGER sift_table_repack_guard ON ddl_command_start WHEN TAG IN ('CREATE TRIGGER') EXECUTE FUNCTION sift.table_ddl_check();

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

-- A sift_col index whose columns carry the default class sift.col_ops (CREATE INDEX .. USING sift_col (col)) answers the filters that return rows as well as the
-- aggregates: the family of the default class has the operators of every typed class (docs/COL-SCAN.md), so the planner and the executor match WHERE id = 5,
-- IN lists, ranges and LIKE 'abc%' to such a column. The classes are catalog entries over the same bytes: an index built before gains this with no REINDEX.
DO $$
DECLARE
	r record;
BEGIN
	FOR r IN SELECT o.amopstrategy AS strat, n.nspname, p.oprname, o.amoplefttype::regtype AS lt, o.amoprighttype::regtype AS rt
			 FROM pg_amop o JOIN pg_opfamily f ON f.oid = o.amopfamily JOIN pg_am a ON a.oid = f.opfmethod
			 JOIN pg_operator p ON p.oid = o.amopopr JOIN pg_namespace n ON n.oid = p.oprnamespace
			 WHERE a.amname = 'sift_col' AND f.opfname <> 'col_ops' ORDER BY f.opfname, o.amoplefttype, o.amoprighttype, o.amopstrategy
	LOOP
		EXECUTE format('ALTER OPERATOR FAMILY sift.col_ops USING sift_col ADD OPERATOR %s %I.%s (%s, %s)', r.strat, r.nspname, r.oprname, r.lt, r.rt);
	END LOOP;
END
$$;

-- Maintenance state of an index: segments and sealed write buffers waiting for promotion, running (a promotion, merge, VACUUM or verify holds
-- the maintenance lock; NULL on a standby) and, when its last background step failed, the number of failures in a row, when, the earliest the
-- background tries again, the SQLSTATE and the message (the log has them too, this keeps the last). NULL while a REINDEX holds the index.
CREATE FUNCTION sift.worker_state(index regclass,
								  OUT segments int4, OUT sealed int4, OUT running bool, OUT failures int4,
								  OUT failed_at timestamptz, OUT next_retry timestamptz, OUT error_code text, OUT last_error text)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_worker_state' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.worker_state(regclass) IS 'Maintenance state of an index: segments and sealed write buffers waiting for promotion, whether a step runs now, and the last background failure with its retry time.';

-- One row per sift and sift_col index (not the storage-less index of a partitioned table) of the database whose table the caller can read; due: why a sift_col index would be rewritten now
-- (sift.col_maintenance_due). A failed step of a sift_col worker is kept like a BM25 one.
CREATE VIEW sift.workers AS
	SELECT c.oid::regclass AS index, a.amname::text AS kind, w.segments, w.sealed, w.running, w.failures, w.failed_at, w.next_retry,
		   w.error_code, w.last_error,
		   CASE WHEN a.amname = 'sift_col' THEN sift.col_maintenance_due(c.oid) END AS due
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname IN ('sift', 'sift_col')
	JOIN pg_index i ON i.indexrelid = c.oid
	CROSS JOIN LATERAL sift.worker_state(c.oid) w
	WHERE c.relkind = 'i' AND has_table_privilege(i.indrelid, 'SELECT') AND w.segments IS NOT NULL;
COMMENT ON VIEW sift.workers IS 'One row per sift and sift_col index the caller can read: its maintenance state (sift.worker_state) and, for a sift_col index, why it would be rewritten now.';
-- it lists what the caller can read (has_table_privilege on the table), so every role may select from it
GRANT SELECT ON sift.workers TO PUBLIC;

-- What a DBA watches on a sift_col index of a large table, one row per index the caller can read: sizes (the file, the segments, the free and the
-- reusable blocks), segments against the directory limit, units listed against units covered (more than 1 is the overlap of promoted segments),
-- the segments and rows a VACUUM left dead, the rows waiting in write buffers, the state of the maintenance (sift.worker_state) and the chain of
-- sift.col_maintenance_status (the last VACUUM, why a rewrite is due and what blocks it). Each query reads the directory and the header of every segment.
CREATE VIEW sift.col_index_status AS
	SELECT c.oid::regclass AS index, m.heap AS "table",
		   pg_relation_size(c.oid) AS index_bytes, x.bytes AS segment_bytes,
		   x.free_blocks * bs.n AS free_bytes, x.reusable_blocks * bs.n AS reusable_bytes,
		   x.rows AS segment_rows, x.segments, x.max_segments, s.largest_segment_bytes, x.units, s.listed_units,
		   s.dirty_segments, s.dead_rows, round(100.0 * s.dead_rows / NULLIF(x.rows, 0), 2) AS dead_pct,
		   s.buffer_rows, w.sealed AS sealed_buffers, w.running, w.failures, w.next_retry,
		   m.last_vacuum, m.changed_rows, m.vacuum_at, m.rewrite_due AS due, m.last_request, m.blocked_by
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname = 'sift_col'
	JOIN pg_index i ON i.indexrelid = c.oid
	CROSS JOIN (SELECT current_setting('block_size')::int8 AS n) bs
	CROSS JOIN LATERAL sift.col_index_info(c.oid) x
	CROSS JOIN LATERAL sift.worker_state(c.oid) w
	CROSS JOIN LATERAL sift.col_maintenance_status(c.oid) m
	CROSS JOIN LATERAL (SELECT max(bytes) FILTER (WHERE kind = 'immutable') AS largest_segment_bytes,
							   sum(units) AS listed_units, count(*) FILTER (WHERE dirty) AS dirty_segments,
							   coalesce(sum(dead_rows), 0) AS dead_rows,
							   coalesce(sum(rows) FILTER (WHERE kind <> 'immutable'), 0) AS buffer_rows
						FROM sift.col_segments(c.oid)) s
	WHERE has_table_privilege(i.indrelid, 'SELECT') AND w.segments IS NOT NULL;
COMMENT ON VIEW sift.col_index_status IS 'One row per sift_col index the caller can read, for the DBA of a large table: file, segment, free and reusable bytes, segments against the limit, rows in the segments (dead ones included), units listed against covered, dirty segments and dead rows, rows in write buffers, the maintenance state and the chain of sift.col_maintenance_status (last VACUUM, why a rewrite is due, what blocks it).';
GRANT SELECT ON sift.col_index_status TO PUBLIC;

-- Logical replication of a sift_table needs pg_sift in shared_preload_libraries (docs/COL-TAM.md section 5): a custom resource manager can only
-- be registered at preload time. Without it the first DML on a published table fails; this trigger refuses CREATE/ALTER PUBLICATION instead, when
-- the publication would carry a sift_table (wal_level = logical, a permanent table, a publication that publishes inserts, updates or deletes).
-- It fires for every role that creates a publication and reads shared_preload_libraries, which only roles with pg_read_all_settings may examine: it
-- runs as the extension's owner (the check of who may publish what is the server's).
CREATE FUNCTION sift.publication_check() RETURNS event_trigger
	LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $fn$
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

COMMENT ON FUNCTION sift.worker_state(regclass) IS 'Maintenance state of one sift or sift_col index: directory entries, sealed write buffers, whether maintenance runs and, when the last background step failed, the failures in a row, when, the earliest retry, the SQLSTATE and the message. Read through sift.workers.';
COMMENT ON VIEW sift.workers IS 'One row per sift and sift_col index whose table you can read: segments, sealed write buffers, whether maintenance runs, the failures in a row of the background, when, the next retry, the SQLSTATE and the last error, and why a sift_col index would be rewritten (due).';

-- sift.approx_count_distinct(x): an estimate of the number of distinct values of x, from a HyperLogLog sketch of 16,384 registers (relative standard error 0.81%).
-- A different function from count(DISTINCT x), which stays exact in every plan: nothing rewrites one into the other and no setting changes it. NULLs are ignored,
-- no rows give 0; every plan gives the same number for the same rows. The reloption hll_columns of a sift_col index keeps a sketch in each unit, which a scan reads
-- instead of the rows.
CREATE FUNCTION sift.approx_count_distinct_trans(internal, anyelement) RETURNS internal
	AS 'MODULE_PATHNAME', 'sift_approx_trans' LANGUAGE C IMMUTABLE PARALLEL SAFE;
CREATE FUNCTION sift.approx_count_distinct_combine(internal, internal) RETURNS internal
	AS 'MODULE_PATHNAME', 'sift_approx_combine' LANGUAGE C IMMUTABLE PARALLEL SAFE;
CREATE FUNCTION sift.approx_count_distinct_serial(internal) RETURNS bytea
	AS 'MODULE_PATHNAME', 'sift_approx_serial' LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;
CREATE FUNCTION sift.approx_count_distinct_deserial(bytea, internal) RETURNS internal
	AS 'MODULE_PATHNAME', 'sift_approx_deserial' LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;
CREATE FUNCTION sift.approx_count_distinct_final(internal) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_approx_final' LANGUAGE C IMMUTABLE PARALLEL SAFE;
CREATE AGGREGATE sift.approx_count_distinct(anyelement) (
	SFUNC = sift.approx_count_distinct_trans, STYPE = internal, COMBINEFUNC = sift.approx_count_distinct_combine,
	SERIALFUNC = sift.approx_count_distinct_serial, DESERIALFUNC = sift.approx_count_distinct_deserial,
	FINALFUNC = sift.approx_count_distinct_final, PARALLEL = SAFE);
COMMENT ON FUNCTION sift.approx_count_distinct_trans(internal, anyelement) IS 'Transition function of sift.approx_count_distinct: adds the extended hash of a value to a HyperLogLog sketch.';
COMMENT ON FUNCTION sift.approx_count_distinct_combine(internal, internal) IS 'Combine function of sift.approx_count_distinct: the maximum of each register of two sketches.';
COMMENT ON FUNCTION sift.approx_count_distinct_serial(internal) IS 'Serial function of sift.approx_count_distinct: the sketch as bytea, for the workers of a parallel aggregate.';
COMMENT ON FUNCTION sift.approx_count_distinct_deserial(bytea, internal) IS 'Deserial function of sift.approx_count_distinct: the sketch of a worker.';
COMMENT ON FUNCTION sift.approx_count_distinct_final(internal) IS 'Final function of sift.approx_count_distinct: the estimate of the distinct values, 0 for none.';
COMMENT ON AGGREGATE sift.approx_count_distinct(anyelement) IS 'An estimate of the distinct values of x (relative standard error 0.81%), the same in every plan; count(DISTINCT x) stays exact.';

-- json_build_object with the type lookups of each argument made once per call site, not per call; the same text (test json_build_object). Put sift before pg_catalog in the search_path to take it for every call of the name.
CREATE FUNCTION sift.json_build_object(VARIADIC "any") RETURNS json
	AS 'MODULE_PATHNAME', 'sift_json_build_object' LANGUAGE C STABLE PARALLEL SAFE;
COMMENT ON FUNCTION sift.json_build_object("any") IS 'json_build_object with the type lookups of each argument made once per call site: the same text as the core function, for one or more arguments';
