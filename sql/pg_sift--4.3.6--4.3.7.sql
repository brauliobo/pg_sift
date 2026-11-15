\echo Use "ALTER EXTENSION pg_sift UPDATE TO '4.3.7'" to load this file. \quit

-- The patch release 4.3.7 adds the one function that restores the liveness pages that a VACUUM of 4.2.1 to 4.3.5 damaged in a segment of more than 65,152 superblocks.
-- The script of the next minor from 4.3 must create it as well (CREATE OR REPLACE: a patch release moves the extension version back to 4.3 and forward again).
CREATE OR REPLACE FUNCTION sift.col_repair(index regclass, dry_run bool DEFAULT false,
										   OUT gen int8, OUT damaged_pages int4, OUT units_fixed int4, OUT ndead_before int8, OUT ndead_after int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_repair' LANGUAGE C STRICT VOLATILE;

COMMENT ON FUNCTION sift.col_repair(regclass, bool) IS 'Restores the liveness pages of a sift_col index that a VACUUM of 4.2.1 to 4.3.5 damaged (special space of the first page, lost dirty bits, count of dead rows); one row for each segment it changed, none when nothing was damaged; with dry_run it only reports';
