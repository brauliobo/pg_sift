\echo Use "ALTER EXTENSION pg_sift UPDATE TO '3.1'" to load this file. \quit

-- The segment views list the column format minor (docs/COL.md 2). A function whose result columns change is dropped and created again;
-- nothing depends on these two.

DROP FUNCTION sift.col_segments(regclass);
DROP FUNCTION sift.table_segments(regclass);

-- One row per segment; the immutable-only columns are NULL for write buffers. format: the column format minor of the segment (3 as
-- pg_sift 3.0 writes it, 4 with the type tag and the element cubes; docs/COL.md 2), preagg_bytes: the bytes of its pre-aggregates.
CREATE FUNCTION sift.col_segments(index regclass,
								  OUT gen int8, OUT kind text, OUT rows int8, OUT bytes int8,
								  OUT sb_lo int4, OUT sb_hi int4, OUT units int4, OUT dirty bool,
								  OUT dead_rows int8, OUT extents int4, OUT format int4, OUT preagg_bytes int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_segments' LANGUAGE C STRICT VOLATILE;

-- One row per segment of a sift_table: kind buffer (the row format write buffer; bytes = its pages), sealed buffer (no more rows,
-- waiting to become columns), columns (a column segment: rows including dead ones, units, dead rows, format = its column format
-- minor, as sift.col_segments) or tombstones.
CREATE FUNCTION sift.table_segments(tbl regclass,
									OUT gen int8, OUT kind text, OUT rows int8, OUT bytes int8,
									OUT sb_lo int4, OUT sb_hi int4, OUT units int4, OUT dead_rows int8, OUT format int4)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_table_segments' LANGUAGE C STRICT VOLATILE;
