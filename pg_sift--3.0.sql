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

-- One row per segment; the section columns are NULL for mutable segments (extents and dead_docs too).
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
									OUT mutable_segments int4, OUT leaves int4, OUT maintenance_busy bool,
									OUT simd text)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_col_index_info' LANGUAGE C STRICT VOLATILE;

-- One row per segment; the immutable-only columns are NULL for mutable segments.
CREATE FUNCTION sift.col_segments(index regclass,
								  OUT gen int8, OUT kind text, OUT rows int8, OUT bytes int8,
								  OUT sb_lo int4, OUT sb_hi int4, OUT units int4, OUT dirty bool,
								  OUT dead_rows int8, OUT extents int4)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_segments' LANGUAGE C STRICT VOLATILE;

-- One row per leaf: its chunks by encoding (const, for, pack, other, text stored inline), their bytes, and the bytes of its dictionaries (text leaves).
CREATE FUNCTION sift.col_leaves(index regclass,
								OUT leaf int4, OUT path text, OUT kind text, OUT type text,
								OUT const_chunks int8, OUT for_chunks int8, OUT pack_chunks int8, OUT codec_chunks int8,
								OUT inline_chunks int8, OUT chunk_bytes int8, OUT dict_bytes int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_leaves' LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sift.col_verify(index regclass, heapallindexed bool DEFAULT false) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_col_verify' LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sift.col_promote(index regclass) RETURNS int4
	AS 'MODULE_PATHNAME', 'sift_col_promote' LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sift.col_merge(index regclass, force bool DEFAULT false) RETURNS int4
	AS 'MODULE_PATHNAME', 'sift_col_merge' LANGUAGE C STRICT VOLATILE;

-- The sift_table table access method (docs/COL-TAM.md): CREATE TABLE ... USING sift_table.
CREATE FUNCTION sift.table_handler(internal) RETURNS table_am_handler
	AS 'MODULE_PATHNAME', 'sift_table_handler' LANGUAGE C STRICT;

CREATE ACCESS METHOD sift_table TYPE TABLE HANDLER sift.table_handler;

-- One row per segment of a sift_table: kind delta (the row format write buffer; bytes = its pages), sealed delta (no more rows,
-- waiting to become columns), columns (a column segment: rows including dead ones, units, dead rows) or tombstones.
CREATE FUNCTION sift.table_segments(tbl regclass,
									OUT gen int8, OUT kind text, OUT rows int8, OUT bytes int8,
									OUT sb_lo int4, OUT sb_hi int4, OUT units int4, OUT dead_rows int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_table_segments' LANGUAGE C STRICT VOLATILE;

-- The space of a sift_table: relation_blocks (the file), high_water (blocks ever used), free_blocks (on the free list, of which
-- reusable_blocks can be taken now, the rest wait for snapshots that may still read them), free_extents.
CREATE FUNCTION sift.table_space(tbl regclass,
								 OUT relation_blocks int8, OUT high_water int8, OUT free_blocks int8, OUT reusable_blocks int8,
								 OUT free_extents int8)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_table_space' LANGUAGE C STRICT VOLATILE;
