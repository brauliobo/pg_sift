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
