\echo Use "ALTER EXTENSION pg_sift UPDATE TO '3.0'" to load this file. \quit

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

-- One row per segment; the immutable-only columns are NULL for write buffers.
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

-- The sift_table table access method (docs/COL-TAM.md): CREATE TABLE ... USING sift_table.
CREATE FUNCTION sift.table_handler(internal) RETURNS table_am_handler
	AS 'MODULE_PATHNAME', 'sift_table_handler' LANGUAGE C STRICT;

CREATE ACCESS METHOD sift_table TYPE TABLE HANDLER sift.table_handler;

-- One row per segment of a sift_table: kind buffer (the row format write buffer; bytes = its pages), sealed buffer (no more rows,
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
