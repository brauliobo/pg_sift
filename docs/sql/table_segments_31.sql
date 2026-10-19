-- Not an extension script: the definition of sift.table_segments that pg_sift--3.0--3.1.sql takes in place of its own (the 3.1 script of
-- feat/v31-table declares it with 9 result columns, ending in format). The write path of 3.1 adds three: unfrozen, min_xid, max_xid.
-- The C function (sift_table_segments) fills the 12 values with or without them: a script that declares fewer columns still works.

DROP FUNCTION sift.table_segments(regclass);

-- One row per segment of a sift_table: kind buffer (the row format write buffer; bytes = its pages), sealed buffer (no more rows,
-- waiting to become columns), columns (a column segment: rows including dead ones, units, dead rows, format = its column format
-- minor, as sift.col_segments; unfrozen: some of its rows are not final and carry their transaction and command, with the smallest
-- and the largest transaction id among them) or tombstones.
CREATE FUNCTION sift.table_segments(tbl regclass,
									OUT gen int8, OUT kind text, OUT rows int8, OUT bytes int8,
									OUT sb_lo int4, OUT sb_hi int4, OUT units int4, OUT dead_rows int8, OUT format int4,
									OUT unfrozen bool, OUT min_xid xid, OUT max_xid xid)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_table_segments' LANGUAGE C STRICT VOLATILE;
