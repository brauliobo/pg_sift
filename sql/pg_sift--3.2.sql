\echo Use "CREATE EXTENSION pg_sift" to load this file. \quit

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
-- NULL when it is not due. automatic: as the server's own rewrite, which leaves an index over sift.vacuum_rewrite_limit megabytes alone.
CREATE FUNCTION sift.col_maintenance_due(index regclass, automatic bool DEFAULT true) RETURNS text
	AS 'MODULE_PATHNAME', 'sift_col_maintenance_due' LANGUAGE C STRICT VOLATILE;

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
-- reusable_blocks can be taken now, the rest wait for snapshots that may still read them), free_extents.
CREATE FUNCTION sift.table_space(tbl regclass,
								 OUT relation_blocks int8, OUT high_water int8, OUT free_blocks int8, OUT reusable_blocks int8,
								 OUT free_extents int8)
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

CREATE VIEW sift.col_rollup_state AS SELECT * FROM sift.col_rollup_counters();
CREATE VIEW sift.col_rollup_requests AS SELECT * FROM sift.col_rollup_queue();

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

-- Checks that the table can hold the paths and rebuilds its layout (C).
CREATE FUNCTION sift.table_paths_check(tbl regclass, col name, spec text) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_table_paths_check' LANGUAGE C VOLATILE;

-- Declares the paths of column col of the sift_table tbl; an empty or NULL spec removes the declaration.
CREATE FUNCTION sift.table_set_paths(tbl regclass, col name, spec text) RETURNS void
	LANGUAGE plpgsql VOLATILE AS $fn$
BEGIN
	PERFORM sift.table_paths_check(tbl, col, spec);
	IF coalesce(spec, '') = '' THEN
		DELETE FROM sift.table_paths WHERE relid = tbl AND attname = col;
	ELSE
		INSERT INTO sift.table_paths VALUES (tbl, col, spec) ON CONFLICT (relid, attname) DO UPDATE SET spec = EXCLUDED.spec;
	END IF;
END
$fn$;

-- The declaration goes with the table.
CREATE FUNCTION sift.table_paths_drop() RETURNS event_trigger
	LANGUAGE plpgsql AS $fn$
BEGIN
	DELETE FROM sift.table_paths p USING pg_event_trigger_dropped_objects() d
		WHERE d.classid = 'pg_class'::regclass AND d.objsubid = 0 AND p.relid = d.objid;
END
$fn$;

CREATE EVENT TRIGGER sift_table_paths_drop ON sql_drop EXECUTE FUNCTION sift.table_paths_drop();
