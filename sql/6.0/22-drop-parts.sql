-- sql/6.0/22-drop-parts.sql: the imperative retention of a Format 2.0 sift_table.
-- Drops the sealed parts whose newest value of col is strictly older than cutoff, in one commit per partition and without reading or deleting a row. A part that spans the cutoff stays whole
-- (parts_partial); a part that is wholly older but not settled yet, or whose partition has a column delta on col, is left for the next call (parts_unsettled). Needs MAINTAIN on the table.
CREATE OR REPLACE FUNCTION sift.drop_parts_before(
	rel					regclass,
	cutoff				timestamptz,
	col					text DEFAULT NULL,
	OUT parts_dropped	int4,
	OUT rows_dropped	int8,
	OUT bytes_reclaimed	int8,
	OUT parts_partial	int4,
	OUT parts_unsettled	int4)
RETURNS record
	AS 'MODULE_PATHNAME', 'sift_drop_parts_before' LANGUAGE C VOLATILE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp;

COMMENT ON FUNCTION sift.drop_parts_before(regclass, timestamptz, text) IS
	'Drops the sealed parts of a Format 2.0 sift_table whose newest value of col (a NOT NULL time column; default: the TTL column, else the first) is older than cutoff, one commit per partition, no row read or deleted. A part spanning the cutoff stays whole (parts_partial); an unsettled one waits for the next call (parts_unsettled). Needs MAINTAIN. Refused for a sift_col index and during recovery.';
