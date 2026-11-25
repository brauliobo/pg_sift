/*
 * col/f2/lazy.h - the lazy filter driver of a scan: the conjuncts of the filter run in the order the adaptive filter (agg/filter_adapt.h) learned, each on its column's chunk as it is stored, and a
 * column is read only when a conjunct about to run needs it. When no row of a granule is left, the columns of the payload are not read, not decompressed and not decoded; when few rows are (under
 * an eighth of the granule), the payload is decoded for those rows alone.
 *
 * The driver knows nothing of pages and encodings: it calls the reader (ops) to load the chunks of columns of a granule, to read a granule whole, and to run a conjunct on the stored form. The
 * scan of a part binds it to sift_lazy_granule_load and sift_granule_filter_compressed (col/f2/f2_lazy_pg.c).
 *
 * Which of two reads a granule gets follows how many granules had a row left over the last 32: when most do, reading the record in one request is cheaper than a request for each chunk; when
 * few do, the chunks of the first conjunct are read and the rest only if a row survives it.
 */
#ifndef SIFT_F2_LAZY_H
#define SIFT_F2_LAZY_H

#include "core/host.h"

#include "agg/filter_adapt.h"
#include "col/f2/stats.h"

#define SIFT_LF_ROWS_MAX		8192
#define SIFT_LF_WORDS			(SIFT_LF_ROWS_MAX / 64)
#define SIFT_LF_SPARSE_DIV		8		/* a granule with fewer rows left than a row in this many is decoded for those rows */

typedef struct SiftLfOps
{
	/* The chunks of the columns that are not yet loaded for granule g; the bytes read (0: they were there) */
	uint64		(*load) (void *ctx, uint32 g, const int *cols, int ncols);
	/* The record of the granule in one request: its chunks are then loaded; the bytes read; NULL: the reader has no whole read */
	uint64		(*load_whole) (void *ctx, uint32 g);
	/* Test conjunct slot on the rows of active and clear the bits of those that fail; false: the stored form of the chunk cannot be tested (active is not touched) and a decoded test must run it */
	bool		(*eval) (void *ctx, uint32 g, int slot, uint32 nrows, uint64 *active);
	/* A clock in nanoseconds for the granules whose time is read; NULL: no time is read and the priors keep their place */
	double		(*now_ns) (void *ctx);
	/* The bytes of column col's chunk in granule g, for the counters of what was not read; NULL: not counted */
	uint32		(*chunk_len) (void *ctx, uint32 g, int col);
	/* Whether conjunct slot has anything to do in granule g (the zones decided it, or its chunk has exceptions): false leaves it out, its column unread; NULL: every conjunct runs */
	bool		(*runnable) (void *ctx, uint32 g, int slot);
} SiftLfOps;

typedef enum SiftLfMode
{
	SIFT_LF_SKIP,				/* no row is left: nothing of the payload is read */
	SIFT_LF_SPARSE,				/* few rows are: decode them by their index (sift_lf_sparse_rows) */
	SIFT_LF_DENSE				/* decode the granule */
} SiftLfMode;

typedef struct SiftLfResult
{
	SiftLfMode	mode;
	uint32		n_active;
	uint64		undecided;		/* the slots of the conjuncts the stored form could not test: the rows still need them (a decoded test) */
	uint64		filter_bytes;	/* what the conjuncts read */
	bool		dense_read;		/* the granule was read whole */
} SiftLfResult;

typedef struct SiftLfStats
{
	uint64		granules;
	uint64		skipped;		/* granules with no row left after the conjuncts */
	uint64		sparse;
	uint64		dense;
	uint64		filter_bytes;
	uint64		payload_bytes_read;
	uint64		payload_bytes_saved;
	uint64		whole_reads;
	uint64		conjunct_runs;	/* tests that ran */
	uint64		conjunct_skips;	/* tests that did not, as no row was left to give them */
} SiftLfStats;

typedef struct SiftLazyFilter
{
	SiftAdaptiveFilter af;
	const SiftLfOps *ops;
	void	   *ctx;
	int			col[SIFT_AFILT_MAX];
	SiftLfStats stats;
} SiftLazyFilter;

extern void sift_lf_init(SiftLazyFilter *lf, const SiftLfOps *ops, void *ctx, const SiftConjunctInit *init, int n, bool io_cold);

#define SIFT_LF_COLD_LEVEL		2		/* parts of level 2 and up (the cold tier) are read rarely and from disk */
#define SIFT_LF_EXT_REMOTE		1		/* bit 0 of SiftPartDesc.ext_flags (SIFT_PART_EXT_REMOTE): the part is an object of the tier directory */

/* Whether the reads of a part miss the shared buffers, so that a byte of a chunk is worth SIFT_AFILT_W_IO_COLD and not SIFT_AFILT_W_IO_WARM to the rank of a conjunct */
static inline bool
sift_lf_part_is_cold(uint8 level, uint8 ext_flags)
{
	return level >= SIFT_LF_COLD_LEVEL || (ext_flags & SIFT_LF_EXT_REMOTE) != 0;
}

/*
 * Runs the conjuncts on granule g of nrows rows. active holds the rows that are alive (not deleted, visible) and is cut to those that pass the conjuncts the stored form could test; the result
 * says what to do with the payload.
 */
extern SiftLfResult sift_lf_granule(SiftLazyFilter *lf, uint32 g, uint32 nrows, uint64 *active);

/* The columns of the payload of granule g, unless no row is left: the bytes read (0 for SIFT_LF_SKIP, which counts them as saved) */
extern uint64 sift_lf_load_payload(SiftLazyFilter *lf, uint32 g, const int *cols, int ncols, const SiftLfResult *r);

/* The indexes of the rows of active in ascending order: how many (at most nrows) */
extern uint32 sift_lf_sparse_rows(const uint64 *active, uint32 nrows, uint32 *idx);

/* The rows of active */
extern uint32 sift_lf_count(const uint64 *active, uint32 nrows);

/* What a predicate of a column is expected to do before anything is measured, from the statistics of the part: the prior of the conjunct */
extern SiftConjunctInit sift_lf_prior(int col, uint8 barrier_group, const SiftPartColStat *st, uint64 nrows, uint64 nnull, SiftPredShape shape, int nvals, SiftCostClass cost);

#endif							/* SIFT_F2_LAZY_H */
