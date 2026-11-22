/*
 * col/f2/f2_sink_ext.h - the rows of an index of format major 2 taken one batch at a time by the consumer instead of being pushed to a sink (col/f2/f2_sink.h): every granule of every part and the
 * slab of rows not yet in a part, decoded straight to the vectors of agg/vec.h for the leaves the consumer names, in slices of SIFT_VEC_LEN rows, with the rows the deletion vectors, the pending
 * deletes and the transaction's own deletes say are dead left out of live. Beside the columns it gives, for the leaves of a key of a jsonb document, the rows that have the key, and for an array
 * column (SIFT_COLK_GROUP) the layout of the elements of the rows of the slice and the element columns decoded for the whole granule.
 *
 * Visibility of the heap is the consumer's: live is what the index structures know, and a row of it may be a version the heap has since ended (a delete or an update is the index's only at the next VACUUM).
 */
#ifndef SIFT_COL_F2_SINK_EXT_H
#define SIFT_COL_F2_SINK_EXT_H

#include "postgres.h"

#include "utils/rel.h"
#include "utils/snapshot.h"

#include "agg/vec.h"
#include "agg/vexpr.h"
#include "col/f2/f2.h"

#define SIFT_F2_PULL_MAX_COLS	SIFT_VEXPR_MAX_REGS
#define SIFT_F2_PULL_MAX_MASKS	SIFT_VEXPR_MAX_MASKS
#define SIFT_F2_PULL_MAX_CHILD	16

/* A leaf the consumer wants: its decoding */
typedef struct SiftF2PullCol
{
	int			leaf;
	SiftVecType type;
	int16		scale;
} SiftF2PullCol;

/* A boolean the consumer wants: the leaf of a key of a jsonb document, whose chunks keep on which rows the key is there */
typedef struct SiftF2PullMask
{
	int			leaf;
} SiftF2PullMask;

typedef struct SiftF2PullSpec
{
	int			ncols;
	SiftF2PullCol col[SIFT_F2_PULL_MAX_COLS];
	int			nmasks;
	SiftF2PullMask mask[SIFT_F2_PULL_MAX_MASKS];
	int			group_leaf;		/* an array column: the leaf of its lengths (SIFT_CENC_LIST_LEN), -1: none */
	int			nchild;
	SiftF2PullCol child[SIFT_F2_PULL_MAX_CHILD];	/* its element leaves */
} SiftF2PullSpec;

/*
 * The elements of the rows of a slice. layout is of the slice (its parents are the rows of the slice, its offsets those of the elements of the granule); a child column has a value for each
 * element of the granule: child[c] is the column of the spec, data and valid as of a SiftVec of n_elems values (valid: n_elems bits, NULL: all valid). poison: the rows of the slice whose array
 * the columns cannot hold (the document was not an array, a list longer than the index keeps, or the rows come from the slab, which keeps no layout): the consumer reads them from the heap.
 */
typedef struct SiftF2ElemStream
{
	SiftArrayLayout layout;
	uint32		n_elems;
	const void *child_data[SIFT_F2_PULL_MAX_CHILD];
	const uint64 *child_valid[SIFT_F2_PULL_MAX_CHILD];
	const uint64 *poison;
	const uint64 *doc_valid;
} SiftF2ElemStream;

typedef struct SiftF2ExtBatch
{
	uint16		n;
	const SiftVec *col;
	const SiftVexprMaskIn *masks;
	const uint64 *tid;
	const uint64 *live;
	const SiftF2ElemStream *elem;	/* NULL unless the spec names a group */
} SiftF2ExtBatch;

typedef struct SiftF2Pull SiftF2Pull;

/* A scan of the index over the snapshot (the transaction's staged rows are flushed first); every part and every slab */
extern SiftF2Pull *sift_f2_pull_begin(Relation index, Snapshot snap, const SiftF2PullSpec *spec);
/* The next slice, valid until the next call; NULL after the last */
extern const SiftF2ExtBatch *sift_f2_pull_next(SiftF2Pull *p);
extern void sift_f2_pull_end(SiftF2Pull *p);

/* Whether the parts of every partition are one sorted run and no row is in a slab: the order a scan in the order of the key gives (the order of the partitions is that of their keys) */
extern bool sift_f2_pull_single_runs(Relation index);

#endif							/* SIFT_COL_F2_SINK_EXT_H */
