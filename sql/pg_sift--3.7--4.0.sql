\echo Use "ALTER EXTENSION pg_sift UPDATE TO '4.0'" to load this file. \quit

-- Where every run of blocks of the file of a sift_col index lies: the segments (their extents, header page and liveness run), the write buffers, the free list and the free extents.
-- pending: a free extent that a snapshot may still read (a standby with hot_standby_feedback holds it), not reusable yet. Order by first_block to read the file from its start.
CREATE FUNCTION sift.col_extents(index regclass,
								 OUT what text, OUT gen int8, OUT first_block int8, OUT nblocks int8, OUT pending bool)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_extents' LANGUAGE C STRICT VOLATILE;

-- The free space of the file in one row: blocks in use, reusable and pending free blocks, free extents (and the largest), what owns the last used block (tail_*, and the
-- block it ends at), where the shrink that VACUUM runs would end the file (shrink_to) and the blocks of the segments above that it has still to move.
CREATE FUNCTION sift.col_space(index regclass,
							   OUT relation_blocks int8, OUT high_water int8, OUT used_blocks int8,
							   OUT reusable_blocks int8, OUT pending_blocks int8, OUT free_extents int4,
							   OUT largest_free_extent int8, OUT tail_what text, OUT tail_gen int8,
							   OUT tail_end int8, OUT shrink_to int8, OUT shrink_blocks int8)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_col_space' LANGUAGE C STRICT VOLATILE;

COMMENT ON FUNCTION sift.col_extents(regclass) IS 'Every run of blocks of the file of a sift_col index: segment extents, headers and liveness runs, write buffers, the free list and the free extents (pending: a snapshot may still read it).';
COMMENT ON FUNCTION sift.col_space(regclass) IS 'The free space of a sift_col file in one row: used, reusable and pending blocks, free extents, the owner of the last used block, and where the VACUUM shrink would end the file.';

-- The statement trigger of the sift_col option track_changes: it writes a tombstone for the entries of the rows a statement deleted or gave another key. The triggers are created
-- and dropped by the index, never by hand.
CREATE FUNCTION sift.col_track_old_rows() RETURNS trigger
	AS 'MODULE_PATHNAME', 'sift_col_track_old_rows' LANGUAGE C;

-- What the log of a sift_col index can be taken for: the option is on (track_changes), the two triggers are the ones that made the log complete (triggers), the log was made
-- complete (complete), and all of it holds now (heap_free: a read could do without the heap for the rows of this index).
CREATE FUNCTION sift.col_tracking(index regclass, OUT track_changes bool, OUT triggers bool, OUT complete bool, OUT heap_free bool)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_col_tracking' LANGUAGE C STRICT VOLATILE;

-- The immutable segments of a sift_col index with track_changes and the xid before which every transaction that inserted an entry of the segment committed (NULL: the segment does not
-- say, as when a speculative insertion, a concurrent build or a transaction that did not commit wrote one of its rows, and its rows are checked against the heap).
CREATE FUNCTION sift.col_stamps(index regclass, OUT gen bigint, OUT entries bigint, OUT stamp xid)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_stamps' LANGUAGE C STRICT VOLATILE;

COMMENT ON FUNCTION sift.col_stamps(regclass) IS 'The immutable segments of a sift_col index with track_changes and the xid before which every transaction that inserted an entry of the segment committed; NULL when the segment does not say';

COMMENT ON FUNCTION sift.col_track_old_rows() IS 'The statement trigger of the sift_col option track_changes: it writes a tombstone for the entries of the rows a statement deleted or gave another key; created and dropped with the index, never by hand.';
COMMENT ON FUNCTION sift.col_tracking(regclass) IS 'What the tombstone log of a sift_col index with track_changes can be taken for: track_changes is on, the two statement triggers are the ones that made the log complete, the log was made complete, and all of it holds now';
