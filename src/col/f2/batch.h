/*
 * col/f2/batch.h - the policy of a batched bulk insert (COPY, CTAS, INSERT ... SELECT into a sift_table) and the string arena its row buffers copy values into. Portable: core/host.h only.
 *
 * A multi_insert call of nslots rows takes the direct route (rows buffered in the transaction's stage and written as parts of level 1) when it has SIFT_BATCH_MIN_SLOTS rows or more, the
 * table is in no logical publication and has no row trigger; anything else keeps the staged route, which a small INSERT needs (a handful of rows is a slab batch, not a part). The buffer of a
 * partition is written when it holds a flush count of rows: a whole number of granules between SIFT_BATCH_MIN_ROWS and SIFT_BATCH_MAX_ROWS that the memory budget holds.
 */
#ifndef SIFT_F2_BATCH_H
#define SIFT_F2_BATCH_H

#include "core/host.h"

#define SIFT_BATCH_MIN_SLOTS		64
#define SIFT_BATCH_RESET_ROWS		1024
#define SIFT_BATCH_GRANULE_ROWS		8192
#define SIFT_BATCH_MIN_ROWS			(2 * SIFT_BATCH_GRANULE_ROWS)
#define SIFT_BATCH_MAX_ROWS			(8 * SIFT_BATCH_GRANULE_ROWS)
#define SIFT_BATCH_COLD_MIN_ROWS	SIFT_BATCH_GRANULE_ROWS		/* a flush of fewer rows than a granule is not worth a bulk part */
#define SIFT_BATCH_VBLOCK_ROWS		291							/* rows of a virtual block of a sift_table */
#define SIFT_BATCH_STR_CHUNK		(256 * 1024)

typedef enum SiftBatchRoute
{
	SIFT_BATCH_DIRECT = 0,
	SIFT_BATCH_STAGED_SMALL,
	SIFT_BATCH_STAGED_PUBLISHED,
	SIFT_BATCH_STAGED_TRIGGERS
} SiftBatchRoute;

/* published and row_triggers are looked at once for the call, never for a row */
extern SiftBatchRoute sift_batch_route(int nslots, bool published, bool row_triggers);

/* Rows of the buffer of a partition that a budget of bytes holds at row_bytes each: a multiple of a granule, SIFT_BATCH_MIN_ROWS .. SIFT_BATCH_MAX_ROWS, and under a granule (down to SIFT_BATCH_RESET_ROWS) when the budget holds fewer */
extern uint32 sift_batch_flush_rows(uint64 budget_bytes, uint64 row_bytes);

/* The bytes a row of columns of the kinds takes in a buffer: a fixed width for each, string_bytes for each string column; the bookkeeping of a row included */
extern uint64 sift_batch_row_bytes(int ncols, int nstr, uint32 string_bytes);

/* Adds nrows to the count since the arena of the batch was reset; true once SIFT_BATCH_RESET_ROWS are counted (the count starts again) */
extern bool sift_batch_reset_due(uint32 *since, uint32 nrows);

/* The virtual blocks that hold nrows rows, rounded up to a multiple of the reservation unit */
extern uint64 sift_batch_vblocks(uint64 nrows, uint64 unit);

/* Strings copied into 256 KiB chunks: one allocation per chunk, not per string; a string over a quarter of a chunk gets a chunk of its own */
typedef struct SiftStrChunk SiftStrChunk;

typedef struct SiftStrArena
{
	MemoryContext cx;
	SiftStrChunk *chunks;
	char	   *at;
	Size		left;
	uint64		nchunks;
	uint64		bytes;
} SiftStrArena;

extern void sift_str_arena_init(SiftStrArena *a, MemoryContext cx);

/* len + 1 bytes copied from s and a terminating zero */
extern char *sift_str_arena_copy(SiftStrArena *a, const char *s, uint32 len);

/* Gives every chunk back (a memory context that is deleted does it as well) */
extern void sift_str_arena_free(SiftStrArena *a);

#endif							/* SIFT_F2_BATCH_H */
