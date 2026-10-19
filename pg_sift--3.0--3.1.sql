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

CREATE VIEW sift.col_rollup_state AS SELECT * FROM sift.col_rollup_counters();
CREATE VIEW sift.col_rollup_requests AS SELECT * FROM sift.col_rollup_queue();
