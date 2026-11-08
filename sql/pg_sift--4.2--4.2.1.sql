\echo Use "ALTER EXTENSION pg_sift UPDATE TO '4.2.1'" to load this file. \quit

-- The two triggers of an index with track_changes were its dependents, and DROP INDEX CONCURRENTLY refused such an index (it deletes what depends on the index first, which assigns
-- the transaction an xid that the command does not allow). The index now depends on its triggers and the transaction that deletes it removes them. This function makes the
-- existing indexes so (a second run finds nothing to convert); its presence is what tells the library that it was done: until then the library does not find the triggers of an index
-- made by an earlier release (its reads check the heap) and removes no trigger of an index that goes (the triggers of an index it made itself have the new dependency from the start).
-- The script of the next minor from 4.2 must create it as well (CREATE OR REPLACE: a patch release moves the extension version back to 4.2 and forward again).
CREATE OR REPLACE FUNCTION sift.col_track_convert() RETURNS integer
	AS 'MODULE_PATHNAME', 'sift_col_track_convert' LANGUAGE C STRICT;

COMMENT ON FUNCTION sift.col_track_convert() IS 'Makes each sift_col index the dependent of its two track_changes triggers (what 4.2.1 does for the indexes of an older release), so that DROP INDEX CONCURRENTLY can drop it; returns the number of triggers converted';

SELECT sift.col_track_convert();
